# Copyright 2021 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0


class RpkRemoteTool:
    """
    Wrapper around rpk. Runs commands on the Redpanda cluster nodes. 
    """
    def __init__(self, redpanda, node):
        self._redpanda = redpanda
        self._node = node

    def config_init(self, path=None, timeout=30):
        cmd = ['init']
        return self._run_config(cmd, path, timeout)

    def config_set(self, key, value, format=None, path=None, timeout=30):
        cmd = [
            'set',
            f"'{key}'",
            f"'{value}'",
        ]

        if format is not None:
            cmd += ['--format', format]

        return self._run_config(cmd, path=path, timeout=timeout)

    def debug_bundle(self, working_dir):
        # Run the bundle command.  It outputs into pwd, so switch to working dir first
        return self._execute(
            ["cd", working_dir, ";",
             self._rpk_binary(), "debug", "bundle"])

    def cluster_config_force_reset(self, property_name):
        return self._execute([
            self._rpk_binary(), 'cluster', 'config', 'force-reset',
            property_name
        ])

    def cluster_config_lint(self):
        return self._execute([self._rpk_binary(), 'cluster', 'config', 'lint'])

    def tune(self, tuner):
        return self._execute([self._rpk_binary(), 'redpanda', 'tune', tuner])

    def mode_set(self, mode):
        return self._execute([self._rpk_binary(), 'redpanda', 'mode', mode])

    def redpanda_start(self, log_file, additional_args="", env_vars=""):
        return self._execute([
            env_vars,
            self._rpk_binary(), "redpanda", "start", "-v", additional_args,
            ">>", log_file, "2>&1", "&"
        ])

    def _run_config(self, cmd, path=None, timeout=30):
        cmd = [self._rpk_binary(), 'redpanda', 'config'] + cmd

        if path is not None:
            cmd += ['--config', path]

        return self._execute(cmd, timeout=timeout)

    def _execute(self, cmd, timeout=30):
        return self._node.account.ssh_output(
            ' '.join(cmd),
            timeout_sec=timeout,
        ).decode('utf-8')

    def _rpk_binary(self):
        return self._redpanda.find_binary('rpk')
