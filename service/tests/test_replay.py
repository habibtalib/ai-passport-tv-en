import hashlib
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from server import live, public_server
from server.protocol import Kind, Packet, ProtocolError, json_bytes
from server.replay import Replay


class FixedReplay(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'reference.mkv'
        self.path.write_bytes(b'fixed source bytes')
        self.device = '4c11ae30ed48'
        self.channel = next(iter(live.CHANNELS))
        self.env = {'TV_REPLAY_FILE': str(self.path), 'TV_REPLAY_DEVICE': self.device,
                    'TV_REPLAY_CHANNEL': self.channel,
                    'TV_REPLAY_UNTIL': str(int(time.time()) + 600)}

    def replay(self):
        with patch.dict(os.environ, self.env, clear=True):
            return Replay.from_env()

    def session(self, replay, device, valid=True, **extra):
        key = b'k' * 40
        expiry = int(time.time()) + 60
        ticket = f'{device}.{expiry}.' + public_server.sign(key, device, expiry)
        hello = Packet(Kind.HELLO, 0, 0, 0, json_bytes(dict(
            version=1, token=ticket if valid else 'invalid',
            **(dict(channel=self.channel) | extra))))
        session = public_server.Session(key, threading.Event(), lambda _: None, replay)
        with patch.object(public_server, 'receive_packet', return_value=hello), \
             patch.object(session, '_live_session') as play:
            session._session(None)
            play.assert_called_once()
        return session

    def test_disabled_without_operator_configuration(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(Replay.from_env())

    def test_requires_complete_bounded_local_configuration(self):
        for change in ({'TV_REPLAY_DEVICE': ''}, {'TV_REPLAY_DEVICE': 'ED48'},
                       {'TV_REPLAY_CHANNEL': ''}, {'TV_REPLAY_CHANNEL': 'missing-channel'},
                       {'TV_REPLAY_FILE': 'relative.mkv'}, {'TV_REPLAY_FILE': self.tmp.name},
                       {'TV_REPLAY_UNTIL': str(int(time.time()) + 86401)}):
            with self.subTest(change=change), patch.dict(os.environ, self.env | change, clear=True):
                with self.assertRaises(ValueError):
                    Replay.from_env()

    def test_hash_and_target_selection(self):
        replay = self.replay()
        self.assertEqual(replay.sha256, hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertTrue(replay.matches(self.device, self.channel))
        self.assertFalse(replay.matches('000000000001', self.channel))
        self.assertFalse(replay.matches(self.device, 'another-channel'))
        with patch('server.replay.time.time', return_value=replay.until):
            self.assertFalse(replay.matches(self.device, self.channel))

    def test_file_loop_uses_normal_video_audio_pipeline(self):
        replay = self.replay()
        url = next(iter(live.CHANNELS.values()))
        channel = replay.channel(url, 'ffmpeg', 'NetworkOnlyAgent')
        with patch.dict(os.environ, {}, clear=True):
            command = live.source_command(channel.url, 1, 2, channel.ffmpeg,
                                          channel.user_agent, 25, loop=channel.loop_input)
        self.assertEqual(command[command.index('-i') + 1], str(self.path))
        self.assertEqual(command[command.index('-stream_loop') + 1], '-1')
        self.assertNotIn('-user_agent', command)
        self.assertNotIn('-reconnect', command)
        self.assertIn(live.source_graph(25), command)
        self.assertEqual(channel.fps, live.FPS)
        normal = live.LiveChannel(url)
        self.assertFalse(normal.loop_input)
        self.assertEqual(normal.url, url)

    def test_channel_allowlist_is_preserved(self):
        with self.assertRaises(ValueError):
            self.replay().channel('/etc/passwd')

    def test_only_authenticated_target_gets_replay(self):
        replay = self.replay()
        self.assertEqual(self.session(replay, self.device).channel_factory, replay.channel)
        self.assertIs(self.session(replay, '000000000001').channel_factory, live.LiveChannel)
        with self.assertRaises(ProtocolError):
            self.session(replay, self.device, valid=False)

    def test_client_cannot_enable_replay_or_choose_local_file(self):
        session = self.session(None, self.device, replay_file='/etc/passwd', channel='/etc/passwd')
        self.assertIs(session.channel_factory, live.LiveChannel)

    def test_switching_channels_keeps_other_channels_live(self):
        replay = self.replay()
        with patch.dict(public_server.CHANNELS, {'another': 'https://example.invalid/live'}):
            session = self.session(replay, self.device, channel='another')
            self.assertIs(session.channel_factory, live.LiveChannel)

    def test_missing_or_invalid_channel_does_not_implicitly_start_replay(self):
        for channel in (None, 'invalid-channel', 1):
            session = self.session(self.replay(), self.device, channel=channel)
            self.assertIs(session.channel_factory, live.LiveChannel)

    def test_explicit_default_channel_request_matches_only_default_test_channel(self):
        session = self.session(self.replay(), self.device, channel='')
        self.assertEqual(session.channel_factory, session.replay.channel)

    def test_test_channel_label_is_session_local_and_other_channels_are_preserved(self):
        session = self.session(self.replay(), self.device)
        entries = session.session_channels()
        original = public_server.Session(b'k'*40, threading.Event(), lambda _: None).session_channels()
        self.assertEqual([e['id'] for e in entries], [e['id'] for e in original])
        for row, old in zip(entries, original):
            self.assertEqual(row, dict(old, name='Video Test') if row['id']==self.channel else old)
        other = self.session(self.replay(), '000000000001')
        self.assertEqual(other.session_channels(), original)

    def test_expired_replay_returns_to_normal_on_new_session(self):
        replay = self.replay()
        with patch('server.replay.time.time', return_value=replay.until):
            # Same process-wide time mock affects ticket creation and validation;
            # the ticket is fresh, only operator replay configuration has expired.
            session = self.session(replay, self.device)
        self.assertIs(session.channel_factory, live.LiveChannel)


if __name__ == '__main__':
    unittest.main()
