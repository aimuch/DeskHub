from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import flash_bundle as bundle


def image(*, version='0.2.1-dev', project='deskhub', chip=9, boot=False) -> bytes:
    data = bytearray(512)
    data[0] = 0xE9
    struct.pack_into('<H', data, 12, chip)
    if not boot:
        struct.pack_into('<I', data, 32, 0xABCD5432)
        data[48:48 + len(version)] = version.encode('ascii')
        data[80:80 + len(project)] = project.encode('ascii')
    return bytes(data)


def status_identity(**changes):
    return {'device_id': 'deskhub-aabbccddeeff', 'firmware_version': '0.2.1-dev',
            'board_model': bundle.BOARD, 'protocol_version': '1.0', **changes}


def status_result(**changes):
    return status_identity(ui_alive=True, theme='mint', rotation=90, config_revision=3, **changes)


def status_frame(value):
    return json.dumps({'ok': True, 'result': value}).encode() + b'\n'


class StatusSerial:
    """No real serial operations; responses arrive only after each request."""

    def __init__(self, *responses, short_write=False):
        self.responses = list(responses)
        self.pending = bytearray()
        self.written = []
        self.events = []
        self.now = 100.0
        self.short_write = short_write
        self.port = None
        self.dtr = self.rts = True

    def open(self):
        self.events.append(('open', self.port, self.dtr, self.rts))

    def close(self):
        self.events.append(('close',))

    def write(self, raw):
        self.written.append(json.loads(raw))
        response = self.responses.pop(0) if self.responses else b''
        if isinstance(response, Exception):
            raise response
        self.pending.extend(response)
        return len(raw) - 1 if self.short_write else len(raw)

    def read(self, count):
        if self.pending:
            raw = bytes(self.pending[:count])
            del self.pending[:count]
            return raw
        self.now += 0.1
        return b''


class FlashStatusTests(unittest.TestCase):
    def run_status(self, port, *args):
        with patch('serial.Serial', return_value=port) as factory, \
                patch.object(bundle.time, 'monotonic', side_effect=lambda: port.now), \
                patch.object(bundle.subprocess, 'run') as command, \
                patch.object(bundle.metadata, 'version') as version, \
                patch.object(bundle, 'verify_bundle') as verify, \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as errors:
            code = bundle.main(['--status', '--port', 'COM5', *args])
        command.assert_not_called()
        version.assert_not_called()
        verify.assert_not_called()
        factory.assert_called_once_with(port=None, baudrate=115200, timeout=0.1, write_timeout=1.0)
        self.assertEqual(port.events[0], ('open', 'COM5', False, False))
        self.assertEqual(port.events[-1], ('close',))
        return code, output.getvalue(), errors.getvalue()

    def test_status_only_sends_identify_status_without_reset_or_bundle(self):
        private = {'wifi': {'ssid': 'SECRET_SSID', 'password': 'SECRET_PASSWORD'},
                   'token': 'SECRET_TOKEN', 'error': 'SECRET_ERROR', 'echo': 'SECRET_ECHO'}
        serial = StatusSerial(status_frame(status_identity(**private)), status_frame({**status_result(), **private}))
        code, output, errors = self.run_status(serial, '--directory', '/does/not/exist')
        self.assertEqual(code, 0)
        self.assertEqual(serial.written, [{'op': 'identify'}, {'op': 'status'}])
        self.assertEqual(set(json.loads(output)), {'device_id', 'firmware_version', 'ui_alive', 'theme', 'rotation', 'config_revision'})
        self.assertIn('未提供触摸诊断', errors)
        self.assertNotIn('SECRET', output + errors)
        self.assertLess(serial.now, 108.0)

    def test_startup_logs_echoes_and_foreign_frames_are_skipped(self):
        noise = b'ESP-ROM boot SECRET\r\n\xff\n{"op":"identify"}\n[]\n'
        noise += status_frame({'foreign': 'SECRET_ECHO'})
        serial = StatusSerial(noise + status_frame(status_identity()), noise + status_frame(status_result()))
        code, output, errors = self.run_status(serial)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)['device_id'], 'deskhub-aabbccddeeff')
        self.assertNotIn('SECRET', output + errors)

    def test_wrong_identity_protocol_or_version_fails_without_status_request(self):
        for changes in ({'device_id': 'bad\nSECRET'}, {'device_id': None}, {'board_model': 'ESP32-C3'},
                        {'protocol_version': '2'}, {'protocol_version': 1},
                        {'firmware_version': 'bad\nSECRET'}, {'firmware_version': None}):
            with self.subTest(changes=changes):
                serial = StatusSerial(status_frame(status_identity(**changes)))
                code, output, errors = self.run_status(serial)
                self.assertEqual(code, 2)
                self.assertEqual(output, '')
                self.assertEqual(serial.written, [{'op': 'identify'}])
                self.assertNotIn('SECRET', errors)

    def test_status_cannot_change_identity_or_protocol(self):
        for changes in ({'device_id': 'another-device'}, {'firmware_version': '0.2.2-dev'},
                        {'protocol_version': '2'}, {'board_model': 'another-board'}):
            with self.subTest(changes=changes):
                serial = StatusSerial(status_frame(status_identity()), status_frame({**status_result(), **changes}))
                code, output, _ = self.run_status(serial)
                self.assertEqual(code, 2)
                self.assertEqual(output, '')

    def test_invalid_scalar_diagnostics_are_not_printed_as_success(self):
        for changes in ({'ui_alive': 1}, {'theme': 'SECRET'}, {'rotation': True},
                        {'rotation': 45}, {'config_revision': -1}, {'config_revision': True}):
            with self.subTest(changes=changes):
                serial = StatusSerial(status_frame(status_identity()), status_frame({**status_result(), **changes}))
                code, output, errors = self.run_status(serial)
                self.assertEqual(code, 2)
                self.assertEqual(output, '')
                self.assertNotIn('SECRET', errors)

    def test_remote_error_is_local_fixed_message_without_echo(self):
        serial = StatusSerial(b'{"ok":false,"error":{"code":"SECRET_TOKEN","message":"SECRET_SSID"}}\n')
        code, output, errors = self.run_status(serial)
        self.assertEqual(code, 2)
        self.assertEqual(output, '')
        self.assertIn('设备拒绝', errors)
        self.assertNotIn('SECRET', errors)

    def test_frame_and_total_byte_limits_fail_without_partial_success(self):
        for raw in (b'X' * bundle.STATUS_FRAME_BYTES + b'\n',
                    (b'X' * 1023 + b'\n') * 129):
            serial = StatusSerial(raw)
            code, output, _ = self.run_status(serial)
            self.assertEqual(code, 2)
            self.assertEqual(output, '')
            self.assertEqual(serial.written, [{'op': 'identify'}])

    def test_short_request_disconnect_and_truncated_response_fail(self):
        cases = (StatusSerial(status_frame(status_identity()), short_write=True),
                 StatusSerial(OSError('SECRET_PATH')),
                 StatusSerial(b'{"ok":true,"result":'))
        for serial in cases:
            with self.subTest(serial=serial):
                code, output, errors = self.run_status(serial)
                self.assertEqual(code, 2)
                self.assertEqual(output, '')
                self.assertNotIn('SECRET', errors)
                self.assertEqual(serial.written, [{'op': 'identify'}])
                self.assertLessEqual(serial.now, 108.1)

    def test_read_disconnect_after_identity_fails_without_partial_json(self):
        serial = StatusSerial(status_frame(status_identity()), b'')
        original_read = serial.read

        def read(count):
            if len(serial.written) == 2:
                raise OSError('SECRET_REMOTE_TEXT')
            return original_read(count)

        serial.read = read
        code, output, errors = self.run_status(serial)
        self.assertEqual(code, 2)
        self.assertEqual(output, '')
        self.assertNotIn('SECRET', errors)

    def test_two_requests_share_eight_second_deadline(self):
        serial = StatusSerial(status_frame(status_identity()), b'')
        original_write = serial.write

        def write(raw):
            result = original_write(raw)
            if len(serial.written) == 1:
                serial.now += 5.0
            return result

        serial.write = write
        code, output, errors = self.run_status(serial)
        self.assertEqual(code, 2)
        self.assertEqual(output, '')
        self.assertIn('超时', errors)
        self.assertLessEqual(serial.now, 108.1)

    def test_status_requires_explicit_port_and_excludes_check_or_list(self):
        for args in (['--status'], ['--status', '--list', '--port', 'COM5'],
                     ['--status', '--check', '--port', 'COM5'], ['--check', '--port', 'COM5']):
            with redirect_stderr(io.StringIO()), patch('serial.Serial') as factory, self.assertRaises(SystemExit) as error:
                bundle.main(args)
            self.assertEqual(error.exception.code, 2)
            factory.assert_not_called()


class FlashBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='deskhub-usb-package-test-')
        self.root = Path(self.temp.name)
        self.manifest = {'schema_version': 1, 'version': '0.2.1-dev', 'chip': 'esp32s3',
                         'board': bundle.BOARD, 'signed_ota': False,
                         'flash_settings': dict(bundle.SETTINGS), 'files': []}
        values = (image(boot=True), bytes(3072), bytes([255]) * 8192, image())
        for (offset, (name, _)), data in zip(bundle.ARTIFACTS.items(), values):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            self.manifest['files'].append({'offset': hex(offset), 'path': name,
                                           'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def replace_image(self, index, data):
        entry = self.manifest['files'][index]
        (self.root / entry['path']).write_bytes(data)
        entry.update(size_bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        self.save()

    def assert_rejected_before_transport(self):
        with patch.object(bundle.subprocess, 'run') as run, patch.object(bundle.metadata, 'version', return_value='5.3.1'):
            with self.assertRaises(bundle.BundleError):
                bundle.flash(self.root, 'COM5')
            run.assert_not_called()

    def test_offline_check_needs_no_esptool_or_serial_connection(self):
        with patch.object(bundle.subprocess, 'run') as run, patch.object(bundle.metadata, 'version') as version, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(bundle.main(['--directory', str(self.root), '--check']), 0)
        self.assertIn('未连接或刷写设备', output.getvalue())
        run.assert_not_called()
        version.assert_not_called()
        self.assertEqual([image.offset for image in bundle.verify_bundle(self.root).images], list(bundle.ARTIFACTS))

    def test_damaged_and_truncated_image_stop_before_any_transport(self):
        path = self.root / 'deskhub.bin'
        original = path.read_bytes()
        for data in (original[:-1], original[:-1] + b'X'):
            path.write_bytes(data)
            self.assert_rejected_before_transport()

    def test_path_escape_or_windows_absolute_path_rejected(self):
        for value in ('../deskhub.bin', '/tmp/deskhub.bin', 'C:\\deskhub.bin',
                      'app/../deskhub.bin', 'app\\deskhub.bin', './deskhub.bin', 'app//deskhub.bin'):
            self.manifest['files'][-1]['path'] = value
            self.save()
            self.assert_rejected_before_transport()

    def test_symlink_is_not_read_as_a_package_image(self):
        with tempfile.TemporaryDirectory(prefix='deskhub-outside-test-') as directory:
            external = Path(directory) / 'outside.bin'
            external.write_bytes((self.root / 'deskhub.bin').read_bytes())
            (self.root / 'deskhub.bin').unlink()
            (self.root / 'deskhub.bin').symlink_to(external)
            self.assert_rejected_before_transport()

    def test_duplicate_missing_and_extra_image_records_rejected(self):
        original = json.loads(json.dumps(self.manifest['files']))
        variants = (original[:-1], original + [original[0]], [original[0], original[0], *original[2:]])
        for items in variants:
            self.manifest['files'] = items
            self.save()
            self.assert_rejected_before_transport()

    def test_overlapping_image_and_nvs_gap_write_rejected(self):
        # Both ranges would otherwise fit in 16 MiB and have correct hashes.
        # Enlarging boot into the partition table must still be refused.
        self.replace_image(0, image(boot=True) + bytes(0x9000 - 512))
        self.assert_rejected_before_transport()
        self.replace_image(0, image(boot=True))
        self.manifest['files'][1]['offset'] = '0x9000'  # NVS begins here.
        self.save()
        self.assert_rejected_before_transport()

    def test_wrong_manifest_board_chip_settings_and_ota_bundle_rejected(self):
        cases = (('board', 'another-board'), ('chip', 'esp32c3'), ('signed_ota', True),
                 ('flash_settings', {**bundle.SETTINGS, 'flash_size': '4MB'}), ('schema_version', True))
        original = dict(self.manifest)
        for field, value in cases:
            self.manifest = {**original, field: value}
            self.save()
            self.assert_rejected_before_transport()

    def test_wrong_image_chip_project_and_version_rejected_even_with_matching_hash(self):
        for data in (image(chip=0), image(project='other'), image(version='0.2.0-dev'), b'not an ESP image'):
            self.replace_image(3, data)
            self.assert_rejected_before_transport()

    def test_duplicate_json_fields_rejected(self):
        (self.root / 'manifest.json').write_text('{"schema_version":1,"schema_version":1}', encoding='utf-8')
        self.assert_rejected_before_transport()

    def test_wrong_esptool_version_stops_before_probe(self):
        with patch.object(bundle.subprocess, 'run') as run, patch.object(bundle.metadata, 'version', return_value='5.6.0'):
            with self.assertRaises(bundle.BundleError):
                bundle.flash(self.root, 'COM5')
            run.assert_not_called()

    def test_wrong_connected_chip_or_flash_capacity_never_writes(self):
        for output in ('Chip type: ESP32-C3\nDetected flash size: 16MB\n',
                       'Chip type: ESP32-S3 (QFN56)\nDetected flash size: 4MB\n',
                       'Chip type: ESP32-S3\nDetected flash size: Unknown\n'):
            with patch.object(bundle.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout=output)) as run, patch.object(bundle.metadata, 'version', return_value='5.3.1'):
                with self.assertRaises(bundle.BundleError):
                    bundle.flash(self.root, 'COM5')
                self.assertEqual(run.call_count, 1)
                self.assertEqual(run.call_args.args[0][-1], 'flash-id')
                self.assertNotIn('write-flash', run.call_args.args[0])

    def test_probe_failure_or_timeout_never_writes(self):
        for failure in (SimpleNamespace(returncode=2, stdout='Chip type: ESP32-S3\nDetected flash size: 16MB\n'),
                        subprocess.TimeoutExpired(['esptool'], 45)):
            with patch.object(bundle.subprocess, 'run') as run, patch.object(bundle.metadata, 'version', return_value='5.3.1'):
                if isinstance(failure, Exception):
                    run.side_effect = failure
                else:
                    run.return_value = failure
                with self.assertRaises(bundle.BundleError):
                    bundle.flash(self.root, 'COM5')
                self.assertEqual(run.call_count, 1)

    def test_success_uses_only_declared_four_images_and_rom_loader(self):
        completed = [SimpleNamespace(returncode=0, stdout='Chip type: ESP32-S3 (QFN56)\nDetected flash size: 16MB\n'),
                     SimpleNamespace(returncode=0)]
        with patch.object(bundle.subprocess, 'run', side_effect=completed) as run, patch.object(bundle.metadata, 'version', return_value='5.3.1'), redirect_stdout(io.StringIO()):
            result = bundle.flash(self.root, 'COM5')
        self.assertEqual(result.version, '0.2.1-dev')
        self.assertEqual(run.call_count, 2)
        command = run.call_args.args[0]
        self.assertEqual(command[:8], [bundle.sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', 'COM5', '--no-stub'])
        self.assertEqual(command[8:15], ['write-flash', '--flash-mode', 'dio', '--flash-size', '16MB', '--flash-freq', '80m'])
        self.assertEqual(command[15::2], [hex(offset) for offset in bundle.ARTIFACTS])
        self.assertEqual(command[16::2], [str((self.root / value[0]).resolve()) for value in bundle.ARTIFACTS.values()])
        self.assertNotIn('erase-flash', command)
        self.assertNotIn('force', command)
        self.assertNotIn('shell', run.call_args.kwargs)

    def test_failed_write_does_not_report_success(self):
        completed = [SimpleNamespace(returncode=0, stdout='Chip type: ESP32-S3\nDetected flash size: 16MB\n'),
                     SimpleNamespace(returncode=2)]
        with patch.object(bundle.subprocess, 'run', side_effect=completed), patch.object(bundle.metadata, 'version', return_value='5.3.1'), redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bundle.main(['--directory', str(self.root), '--port', 'COM5']), 2)
        self.assertNotIn('刷写及 esptool 镜像校验完成', output.getvalue())
        self.assertIn('未成功', errors.getvalue())

    def test_port_is_required_and_list_never_selects_or_flashes(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            bundle.main(['--directory', str(self.root)])
        self.assertEqual(error.exception.code, 2)
        ports = [SimpleNamespace(device='COM9', vid=0x303A, pid=0x1001, description='Espressif USB'),
                 SimpleNamespace(device='COM2', vid=None, pid=None, description='Other')]
        with patch('serial.tools.list_ports.comports', return_value=ports), patch.object(bundle.subprocess, 'run') as run, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(bundle.main(['--list']), 0)
        self.assertIn('COM9\t303a:1001', output.getvalue())
        self.assertIn('不会自动选择', output.getvalue())
        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
