#!/usr/bin/env python3
"""Socket-free kernel metadata and fixed primary-listener admission tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

CHECKER = None


class PrimaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-primary-')
        self.addCleanup(self.temp.cleanup)
        self.input = Path(self.temp.name) / 'metadata'
        self.status = {'Name': 'flex-tailnet-sh', 'Tgid': '42', 'Pid': '42', 'TracerPid': '0',
                       'Uid': '78666\t78666\t78666\t78666', 'Gid': '20\t20\t20\t20', 'Groups': '',
                       'CapInh': '0000000000000000', 'CapPrm': '0000000000000000',
                       'CapEff': '0000000000000000', 'CapBnd': '0000000000000000',
                       'CapAmb': '0000000000000000', 'NoNewPrivs': '1'}
        self.header = '  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n'

    def invoke(self, command, data, args=(), expected=0, number=0):
        self.input.write_bytes(data.encode() if isinstance(data, str) else data)
        p = subprocess.run([str(CHECKER), command, str(self.input), *map(str, args)], capture_output=True, timeout=5)
        self.assertEqual(p.returncode, expected, p.stderr.decode(errors='replace'))
        self.assertEqual(p.stdout, ('ok %d\n' % number).encode() if expected == 0 else b'')

    def status_text(self, status=None):
        return ''.join(k + ':\t' + v + '\n' for k, v in (self.status if status is None else status).items())

    def stat(self, changes=None, pid=42, comm='flex-tailnet-shim'):
        values = {i: '0' for i in range(3, 53)}
        values.update({3: 'S', 19: '10', 22: '123456', 41: '0'})
        values.update(changes or {})
        return str(pid) + ' (' + comm + ') ' + ' '.join(values[i] for i in range(3, 53)) + '\n'

    def row(self, ipv6=False, owner=78666, inode=123, local=None, remote=None, state='0A'):
        zero = '0' * (32 if ipv6 else 8)
        return '  0: %s:BF60 %s:0000 %s 00000000:00000000 00:00000000 00000000 %s 0 %s 1 00000000 100 0 0 10 0\n' % (
            zero if local is None else local, zero if remote is None else remote, state, owner, inode)

    def test_unprivileged_modern_process_metadata(self):
        self.invoke('status', self.status_text(), (42, 3))
        self.status['UnknownKernelField'] = 'ignored'
        self.invoke('status', self.status_text(), (42, 3))

    def test_every_uid_gid_and_supplementary_group_is_checked(self):
        for field, correct in [('Uid', '78666'), ('Gid', '20')]:
            for index in range(4):
                status = dict(self.status); values = [correct] * 4; values[index] = '0'
                status[field] = ' '.join(values)
                self.invoke('status', self.status_text(status), (42, 3), expected=1)
            for bad in ['', correct, ' '.join([correct] * 5), ' '.join([correct] * 3 + ['-1'])]:
                status = dict(self.status); status[field] = bad
                self.invoke('status', self.status_text(status), (42, 3), expected=1)
        self.status['Groups'] = '20'
        self.invoke('status', self.status_text(), (42, 3), expected=1)

    def test_all_capability_sets_must_be_empty(self):
        for key in ['CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb']:
            for value in ['0000000000000001', 'ffffffffffffffff', '0', '-0000000', '0000000g']:
                status = dict(self.status); status[key] = value
                self.invoke('status', self.status_text(status), (42, 3), expected=1)

    def test_modern_kernel_fields_required_when_supported(self):
        for field in ['NoNewPrivs', 'CapAmb']:
            status = dict(self.status); del status[field]
            self.invoke('status', self.status_text(status), (42, 3), expected=1)
        self.status['NoNewPrivs'] = '0'
        self.invoke('status', self.status_text(), (42, 3), expected=1)
        self.invoke('status', self.status_text(), (42, 0), expected=1)

    def test_old_kernel_omissions_do_not_waive_bounding_set(self):
        del self.status['NoNewPrivs']; del self.status['CapAmb']
        self.invoke('status', self.status_text(), (42, 0))
        self.status['CapBnd'] = 'ffffffff'
        self.invoke('status', self.status_text(), (42, 0), expected=1)

    def test_missing_duplicate_and_wrong_process_fields_refuse(self):
        for field in self.status:
            if field == 'Name':
                continue
            status = dict(self.status); del status[field]
            self.invoke('status', self.status_text(status), (42, 3), expected=1)
            self.invoke('status', self.status_text() + field + ': ' + self.status[field] + '\n', (42, 3), expected=1)
        for field in ['Tgid', 'Pid', 'TracerPid']:
            status = dict(self.status); status[field] = '43'
            self.invoke('status', self.status_text(status), (42, 3), expected=1)

    def test_stat_identity_start_and_scheduler_fields(self):
        self.invoke('stat', self.stat(), (42,), number=123456)
        self.invoke('stat', self.stat(comm='weird ) (name)'), (42,), number=123456)
        self.invoke('stat', self.stat(pid=43), (42,), expected=1)
        for field, bad in [(3, 'Z'), (3, 'T'), (3, 'X'), (19, '0'), (19, '-10'), (22, '0'),
                           (22, '-1'), (22, '18446744073709551616'), (41, '1'), (41, '2')]:
            self.invoke('stat', self.stat({field: bad}), (42,), expected=1)

    def test_stat_truncation_and_malformed_comm_refuse(self):
        for text in [self.stat()[:20] + '\n', self.stat().replace('(', '', 1), self.stat().replace(') ', ')'),
                     self.stat().replace('42 (', '42x ('), self.stat()[:-1], self.stat() + '\0', 'x' * 4096 + '\n']:
            self.invoke('stat', text, (42,), expected=1)

    def test_fixed_service_cgroup_v1_and_v2(self):
        self.invoke('cgroup', '0::/system.slice/aetherpd-shim.service\n')
        self.invoke('cgroup', '2:cpu,cpuacct:/system/aetherpd-shim.service\n1:name=systemd:/system/aetherpd-shim.service\n')

    def test_wrong_descendant_duplicate_or_missing_cgroup_refuses(self):
        for text in ['0::/\n', '0::/system.slice/other.service\n', '0::/system.slice/aetherpd-shim.service/child\n',
                     '0::/system.slice/aetherpd-shim.service/../other\n', '2:cpu:/system.slice/aetherpd-shim.service\n',
                     'x::/system.slice/aetherpd-shim.service\n',
                     '0::/system.slice/aetherpd-shim.service\n0::/system.slice/aetherpd-shim.service\n']:
            self.invoke('cgroup', text, expected=1)

    def test_tcp_v4_and_v6_wildcard_listener(self):
        for family in [0, 1]:
            self.invoke('tcp', self.header + self.row(ipv6=bool(family)), (family, 0), number=123)
            self.invoke('tcp', self.header, (family, 0))

    def test_tcp_conflicting_owner_or_second_listener_refuses(self):
        for family in [0, 1]:
            for owner in [0, 20, 65534, -1]:
                self.invoke('tcp', self.header + self.row(ipv6=bool(family), owner=owner), (family, 0), expected=1)
            self.invoke('tcp', self.header + self.row(ipv6=bool(family)) * 2, (family, 0), expected=1)
            self.invoke('tcp', self.header + self.row(ipv6=bool(family)), (family, 999), expected=1)

    def test_tcp_bound_address_remote_and_inode_refuse(self):
        for family in [0, 1]:
            address = '0' * (31 if family else 7) + '1'
            for kw in [dict(local=address), dict(remote=address), dict(inode=0), dict(inode=-1), dict(inode=2**64)]:
                self.invoke('tcp', self.header + self.row(ipv6=bool(family), **kw), (family, 0), expected=1)

    def test_tcp_nonlistener_and_other_ports_do_not_claim_ownership(self):
        self.invoke('tcp', self.header + self.row(state='01', inode=321), (0, 0))
        self.invoke('tcp', self.header + self.row().replace(':BF60', ':BF61'), (0, 0))
        self.invoke('tcp', self.header + self.row(state='06', inode=0) + self.row(), (0, 0), number=123)

    def test_tcp_truncation_oversize_and_wrong_address_width_refuse(self):
        for value in [b'', b'no header\n', (self.header + 'broken\n').encode(),
                      (self.header + self.row()).encode()[:-1], b'x' * 65536,
                      (self.header + self.row().replace(':BF60', ':xxxxx')).encode()]:
            self.invoke('tcp', value, (0, 0), expected=1)
        self.invoke('tcp', self.header + self.row(ipv6=True), (0, 0), expected=1)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--checker', required=True, type=Path)
    args, remaining = p.parse_known_args(); CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *remaining])
