#!/usr/bin/python3
"""
This script enables testing local modifications to .circleci/config.yml
without committing changes to the repo. This is useful for testing.

Requirements:

* CircleCI utility set up with valid auth token
* All other files except .circleci/config.yml are pushed to a remote tracking branch
* CircleCI is set up for the remote of the remote tracking branch

* Python 3.5+
* Python requests library
* GIT
* PyYAML

"""

from collections import namedtuple
import re
import subprocess
import requests
import yaml
import os.path

RemoteInfo = namedtuple('RemoteInfo', ['service', 'organization', 'repo_name'])


def parse_url_to_remote_info(url):
    match = re.match(r'^((?P<user>[^@]+)@)?'
                     r'(?P<hostname>[^:]+):'
                     r'(?P<organization>[^/]+)/'
                     r'(?P<repo_name>.+).git?$',
                     url)
    if not match or match.group('hostname') != 'github.com':
        raise Exception("Could not parse remote URL " + url)
    return RemoteInfo('github', match.group('organization'), match.group('repo_name'))


def execute(*args):
    result = subprocess.run(args,
                            stdout=subprocess.PIPE,
                            check=True)
    return result.stdout.decode('utf-8').strip()


def get_remote_tracking_branch():
    return execute('git', 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}')


def get_url_for_remote(remote_name):
    return execute('git', 'remote', 'get-url', '--push', remote_name)


def get_current_commit():
    return execute('git', 'rev-parse', 'HEAD')


def get_config_yml(top_dir):
    with open(os.path.join(top_dir, '.circleci/config.yml'), 'rb') as config_yml:
        return config_yml.read()


def get_auth_token():
    circle_ci_cli_config = os.path.expanduser('~/.circleci/cli.yml')
    with open(circle_ci_cli_config, 'rb') as config:
        config_yaml = yaml.load(config)
        return config_yaml['token']


def main():
    top_dir = execute('git', 'rev-parse', '--show-toplevel')
    subprocess.run(('circleci', 'config', 'validate'), cwd=top_dir, check=True)
    config_yml_string = get_config_yml(top_dir)

    remote_tracking_branch = get_remote_tracking_branch()
    match = re.match(r'^(?P<remote_name>[^/]+)/(?P<remote_branch_name>[^/]+)$', remote_tracking_branch)
    if not match:
        raise Exception('Cannot parse remote tracking branch ' + remote_tracking_branch)
    remote_branch_name = match.group('remote_branch_name')
    remote_url = get_url_for_remote(match.group('remote_name'))
    remote_info = parse_url_to_remote_info(remote_url)
    revision = get_current_commit()
    auth_token = get_auth_token()
    url = 'https://circleci.com/api/v1.1/project/{service}/{organization}/{repo_name}/tree/{branch_name}'.format(
        service=remote_info.service,
        organization=remote_info.organization,
        repo_name=remote_info.repo_name,
        branch_name=remote_branch_name,
    )
    response = requests.post(url,
                             auth=(auth_token, ''),
                             data={
                                 'revision': revision,
                                 'notify': 'false',
                                 'config': config_yml_string,
                             })
    response_data = response.json()
    response.raise_for_status()
    print('Build triggered as', response_data['build_url'])


if __name__ == "__main__":
    exit(main())
