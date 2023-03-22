#!/usr/bin/env python3
#
# Copyright (c) 2019 MariaDB Corporation Ab
# Copyright (c) 2023 MariaDB plc, Finnish Branch
#
#  Use of this software is governed by the Business Source License included
#  in the LICENSE.TXT file and at www.mariadb.com/bsl11.
#
#  Change Date: 2027-03-14
#
#  On the date above, in accordance with the Business Source License, use
#  of this software will be governed by version 2 or later of the General
#  Public License.
#

import sys
import os
from argparse import ArgumentParser
import subprocess
import time
import getpass

help_text = '''
This script reads the output generated by make-docker-env.py.
It can deploy and issue remote docker-compose commands, as
well as any regular command line commands (like 'ls').
'''

#### print to stderr
def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


settings = None # make this a class

#### function main
def main():
    parser = ArgumentParser(help_text)
    parser.add_argument("command", nargs='?', default = "ps",
                        help="Docker-compose command to run. In addition the commands deploy and purge are accepted.")

    parser.add_argument("-c", "--cli",
                        help="Run any normal cli command, like 'ls -l'.")

    parser.add_argument("-b", "--basedir", default = "./docker-env",
                        help="Base directory to generate the docker environment. Default ./docker-env")

    parser.add_argument("-r", "--remote_user", default = "tester",
                        help="The remote user under whose home directory the docker files are copied. Default is 'tester'.")

    parser.add_argument("-s", "--silent", action="store_true", default=False,
                        help="Silent mode. There will still be some output")

    parser.add_argument("-H", "--host", type=str, action='append',
                        help="limit the action to these hosts")

    global settings

    settings = parser.parse_args()

    settings.local_user  = getpass.getuser()

    all_hosts = [f for f in os.listdir(settings.basedir) if os.path.isdir(os.path.join(settings.basedir, f))]

    if settings.host == None:
        settings.selected_hosts = all_hosts
    else:
        settings.selected_hosts = settings.host
        for h in settings.selected_hosts:
            if not h in all_hosts:
                eprint("Error: Host %s is not a subdirectory of %s/" % (h, settings.basedir))
                sys.exit(13)

    settings.selected_hosts.sort()

    if settings.cli:
        return execute_regular_command(settings.cli)
    else:
        return get_command(settings.command)()


#### function get_command. Make a docker-compose command (lambda), or commands deploy or purge
def get_command(command):
    commands = {
        "deploy": deploy_command,
        "purge":  purge_command,
        }
    return commands.get(command, lambda : compose_command(command))


#### function completed_process_print. Print stdout and stderr of a CompletedProcess
def completed_process_print(cp):
    eout = False

    if cp.returncode:
        eout = True
        print(cp.host + ':')
        eprint(cp.returncode, cp.stderr.decode('utf-8'))

    if not settings.silent and len(cp.stdout):
        if not eout:
            print(cp.host + ':')
        print(cp.stdout.decode('utf-8'))


#### function execute_regular_command, any command you would notmally run on command line
#### Executed in the home directory of the remote user.
def execute_regular_command(cmd):
    for host in settings.selected_hosts:
        completed_process = subprocess.run(["ssh", "%s@%s" % (settings.remote_user, host), cmd],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE);
        completed_process.host = host
        completed_process_print(completed_process)


#### function execute_command Executes a docker-compose command
def compose_command(command):

    cmd = "docker-compose %s"  % command

    for host in settings.selected_hosts:
        ssh_cmd = "cd docker/%s; %s" % (settings.local_user, cmd)
        stdout=""
        stderr=""
        completed_process = subprocess.run(["ssh", "%s@%s" %(settings.remote_user, host), ssh_cmd],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE);

        completed_process.host = host
        completed_process_print(completed_process)


#### function deploy_command
def deploy_command():
    for host in settings.selected_hosts:
        source = settings.basedir + '/' + host + '/*'

        target_dir = "/home/%s/docker/%s" % (settings.remote_user, settings.local_user)

        target_scp = "%s@%s:%s/" % (settings.remote_user, host, target_dir)

        completed_process = subprocess.run(["ssh", "%s@%s" % (settings.remote_user, host), "mkdir -p %s" % target_dir],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE);

        completed_process.host = host
        completed_process_print(completed_process)

        completed_process = subprocess.run(["scp -rp " + source + ' ' + target_scp],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,
                                shell=True)


        completed_process.host = host
        completed_process_print(completed_process)


#### function purge, stop docker, docker prune verything and remove docker deployment files
#### Only purges the stuff associated with the current user. TODO, add purge everything (everyone) option.
def purge_command():
    compose_command("down")
    for host in settings.selected_hosts:
        completed_process = subprocess.run(["ssh", "%s@%s" % (settings.remote_user, host), "docker system prune --volumes --force"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE);

        completed_process.host = host
        completed_process_print(completed_process)

    for host in settings.selected_hosts:
        completed_process = subprocess.run(["ssh", "%s@%s" % (settings.remote_user, host),
                               "rm -rf /home/" + settings.remote_user + "/docker/" + settings.local_user],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE);

        completed_process.host = host
        completed_process_print(completed_process)


if __name__ == "__main__":
    main()

