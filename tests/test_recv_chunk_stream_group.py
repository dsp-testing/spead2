# Copyright 2023 National Research Foundation (SARAO)
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import collections.abc
import threading
import time

import numpy as np
import pytest

import spead2
import spead2.recv as recv
import spead2.send as send

from tests.test_recv_chunk_stream import (
    CHUNK_PAYLOAD_SIZE, HEAP_PAYLOAD_SIZE, HEAPS_PER_CHUNK, place_plain_llc
)

STREAMS = 4
LOSSY_PARAM = pytest.param(recv.ChunkStreamGroupConfig.EvictionMode.LOSSY, id="lossy")
LOSSLESS_PARAM = pytest.param(recv.ChunkStreamGroupConfig.EvictionMode.LOSSLESS, id="lossless")


class TestChunkStreamGroupConfig:
    def test_default_construct(self):
        config = recv.ChunkStreamGroupConfig()
        assert config.max_chunks == config.DEFAULT_MAX_CHUNKS
        assert config.eviction_mode == recv.ChunkStreamGroupConfig.EvictionMode.LOSSY

    def test_zero_max_chunks(self):
        with pytest.raises(ValueError):
            recv.ChunkStreamGroupConfig(max_chunks=0)

    def test_max_chunks(self):
        config = recv.ChunkStreamGroupConfig(max_chunks=3)
        assert config.max_chunks == 3
        config.max_chunks = 4
        assert config.max_chunks == 4

    def test_eviction_mode(self):
        EvictionMode = recv.ChunkStreamGroupConfig.EvictionMode
        config = recv.ChunkStreamGroupConfig(eviction_mode=EvictionMode.LOSSLESS)
        assert config.eviction_mode == EvictionMode.LOSSLESS
        config.eviction_mode = EvictionMode.LOSSY
        assert config.eviction_mode == EvictionMode.LOSSY


class TestChunkStreamRingGroupSequence:
    """Test that ChunkStreamRingGroup behaves like a sequence."""
    @pytest.fixture
    def config(self):
        return spead2.recv.ChunkStreamGroupConfig()

    @pytest.fixture
    def data_ring(self):
        return spead2.recv.ChunkRingbuffer(4)

    @pytest.fixture
    def free_ring(self):
        ring = spead2.recv.ChunkRingbuffer(4)

    def make_group(self, n_streams):
        group = spead2.recv.ChunkStreamRingGroup(
            spead2.recv.ChunkStreamGroupConfig(),
            spead2.recv.ChunkRingbuffer(4),
            spead2.recv.ChunkRingbuffer(4)
        )
        streams = []
        for _ in range(n_streams):
            streams.append(
                group.emplace_back(
                    spead2.ThreadPool(),
                    spead2.recv.StreamConfig(),
                    spead2.recv.ChunkStreamConfig(place=place_plain_llc)
                )
            )
        return group, streams

    def test_len(self):
        group, _ = self.make_group(5)
        assert len(group) == 5

    def test_getitem_simple(self):
        group, streams = self.make_group(3)
        assert group[0] is streams[0]
        assert group[1] is streams[1]
        assert group[2] is streams[2]

    def test_getitem_wrap(self):
        group, streams = self.make_group(3)
        assert group[-1] is streams[-1]
        assert group[-2] is streams[-2]
        assert group[-3] is streams[-3]

    def test_getitem_bad(self):
        group, streams = self.make_group(3)
        with pytest.raises(IndexError):
            group[3]
        with pytest.raises(IndexError):
            group[-4]

    def test_getitem_slice(self):
        group, streams = self.make_group(5)
        assert group[1:3] == streams[1:3]
        assert group[4:0:-2] == streams[4:0:-2]
        assert group[1:-1:2] == streams[1:-1:2]

    def test_iter(self):
        group, streams = self.make_group(5)
        assert list(group) == streams

    def test_reversed(self):
        group, streams = self.make_group(5)
        assert list(reversed(group)) == list(reversed(streams))

    def test_contains(self):
        group, streams = self.make_group(2)
        assert streams[0] in group
        assert streams[1] in group
        assert None not in group

    def test_count(self):
        group, streams = self.make_group(2)
        assert group.count(streams[0]) == 1
        assert group.count(streams[1]) == 1
        assert group.count(group) == 0

    def test_index(self):
        group, streams = self.make_group(2)
        assert group.index(streams[0]) == 0
        assert group.index(streams[1]) == 1
        assert group.index(streams[1], 1, 2) == 1
        with pytest.raises(ValueError):
            group.index(None)
        with pytest.raises(ValueError):
            group.index(streams[0], 1)

    def test_registered(self):
        assert issubclass(spead2.recv.ChunkStreamRingGroup, collections.abc.Sequence)


class TestChunkStreamRingGroup:
    @pytest.fixture
    def data_ring(self):
        return spead2.recv.ChunkRingbuffer(4)

    @pytest.fixture
    def free_ring(self):
        ring = spead2.recv.ChunkRingbuffer(4)
        while not ring.full():
            ring.put(
                recv.Chunk(
                    present=np.zeros(HEAPS_PER_CHUNK, np.uint8),
                    data=np.zeros(CHUNK_PAYLOAD_SIZE, np.uint8)
                )
            )
        return ring

    @pytest.fixture
    def queues(self):
        return [spead2.InprocQueue() for _ in range(STREAMS)]

    @pytest.fixture(params=[LOSSY_PARAM, LOSSLESS_PARAM])
    def eviction_mode(self, request):
        return request.param

    @pytest.fixture
    def group(self, eviction_mode, data_ring, free_ring, queues):
        group_config = recv.ChunkStreamGroupConfig(max_chunks=4, eviction_mode=eviction_mode)
        group = recv.ChunkStreamRingGroup(group_config, data_ring, free_ring)
        # max_heaps is artificially high to make test_packet_too_old work
        config = spead2.recv.StreamConfig(max_heaps=128)
        chunk_stream_config = spead2.recv.ChunkStreamConfig(
            items=[0x1000, spead2.HEAP_LENGTH_ID],
            max_chunks=4,
            place=place_plain_llc,
        )
        for queue in queues:
            group.emplace_back(
                spead2.ThreadPool(),
                config=config,
                chunk_stream_config=chunk_stream_config
            )
        for stream, queue in zip(group, queues):
            stream.add_inproc_reader(queue)
        yield group
        group.stop()

    @pytest.fixture
    def send_stream(self, queues):
        return send.InprocStream(spead2.ThreadPool(), queues, send.StreamConfig())

    def _send_data(self, send_stream, data, eviction_mode, heaps=None):
        """Send the data.

        To send only a subset of heaps (or to send out of order), pass the
        indices to skip in `heaps`.
        """
        lossy = (eviction_mode == recv.ChunkStreamGroupConfig.EvictionMode.LOSSY)
        data_by_heap = data.reshape(-1, HEAP_PAYLOAD_SIZE)
        ig = spead2.send.ItemGroup()
        ig.add_item(0x1000, 'position', 'position in stream', (), format=[('u', 32)])
        ig.add_item(0x1001, 'payload', 'payload data', (HEAP_PAYLOAD_SIZE,), dtype=np.uint8)
        # In lossy mode the behaviour is inherently non-deterministic.
        # We just feed the data in slowly enough that we expect heaps provided
        # before a sleep to be processed before those after the sleep.
        for i in heaps:
            ig['position'].value = i
            ig['payload'].value = data_by_heap[i]
            heap = ig.get_heap(data='all', descriptors='none')
            send_stream.send_heap(heap, substream_index=i % STREAMS)
            if lossy:
                time.sleep(0.001)

    def _verify(self, group, data, expected_present):
        expected_present = expected_present.reshape(-1, HEAPS_PER_CHUNK)
        chunks = len(expected_present)
        data_by_heap = data.reshape(chunks, HEAPS_PER_CHUNK, -1)

        for i in range(len(expected_present)):
            chunk = group.data_ringbuffer.get()
            assert chunk.chunk_id == i
            np.testing.assert_equal(chunk.present, expected_present[i])
            actual_data = chunk.data.reshape(HEAPS_PER_CHUNK, -1)
            for j in range(HEAPS_PER_CHUNK):
                if expected_present[i, j]:
                    np.testing.assert_equal(actual_data[j], data_by_heap[i, j])
            group.add_free_chunk(chunk)

        # Stopping all the queues should shut down the data ringbuffer
        with pytest.raises(spead2.Stopped):
            group.data_ringbuffer.get()

    def _test_simple(self, group, send_stream, chunks, heaps):
        """Send a given set of heaps (in order) and check that they arrive correctly."""
        rng = np.random.default_rng(seed=1)
        data = rng.integers(0, 256, chunks * CHUNK_PAYLOAD_SIZE, np.uint8)

        def send():
            self._send_data(send_stream, data, group.config.eviction_mode, heaps)
            # Stop all the queues, which should flush everything and stop the
            # data ring.
            for queue in send_stream.queues:
                queue.stop()

        send_thread = threading.Thread(target=send)
        send_thread.start()

        expected_present = np.zeros(chunks * HEAPS_PER_CHUNK, np.uint8)
        expected_present[heaps] = True
        self._verify(group, data, expected_present)

        send_thread.join()

    def test_full_in_order(self, group, send_stream):
        """Send all the data, in order."""
        chunks = 20
        heaps = list(range(chunks * HEAPS_PER_CHUNK))
        self._test_simple(group, send_stream, chunks, heaps)

    def test_missing_stream(self, group, send_stream):
        """Skip sending data to one of the streams."""
        chunks = 20
        heaps = [i for i in range(chunks * HEAPS_PER_CHUNK) if i % STREAMS != 2]
        self._test_simple(group, send_stream, chunks, heaps)

    @pytest.mark.parametrize("eviction_mode", [LOSSLESS_PARAM])
    def test_lossless_late_stream(self, group, send_stream):
        """Send one stream later than the others, to make sure lossless mode really works."""
        chunks = 20
        rng = np.random.default_rng(seed=1)
        data = rng.integers(0, 256, chunks * CHUNK_PAYLOAD_SIZE, np.uint8)
        heaps1 = [i for i in range(chunks * HEAPS_PER_CHUNK) if i % STREAMS != 2]
        heaps2 = [i for i in range(chunks * HEAPS_PER_CHUNK) if i % STREAMS == 2]

        def send():
            self._send_data(send_stream, data, group.config.eviction_mode, heaps1)
            time.sleep(0.01)
            self._send_data(send_stream, data, group.config.eviction_mode, heaps2)
            # Stop all the queues, which should flush everything and stop the
            # data ring.
            for queue in send_stream.queues:
                queue.stop()

        send_thread = threading.Thread(target=send)
        send_thread.start()

        expected_present = np.ones(chunks * HEAPS_PER_CHUNK, np.uint8)
        self._verify(group, data, expected_present)

        send_thread.join()

    def test_unblock_stop(self, group, send_stream):
        """Stop the group without stopping the queues."""
        chunks = 20
        # Leave one stream half-missing, to really jam things up
        n_heaps = chunks * HEAPS_PER_CHUNK
        heaps = [i for i in range(n_heaps) if i < n_heaps // 2 or i % STREAMS != 2]
        rng = np.random.default_rng(seed=1)
        data = rng.integers(0, 256, chunks * CHUNK_PAYLOAD_SIZE, np.uint8)

        self._send_data(send_stream, data, group.config.eviction_mode, heaps)
        time.sleep(0.01)  # Give it time to consume some of the data
        group.stop()

        # We don't care how many chunks we get, as long as the loop
        # terminates.
        for i, chunk in enumerate(group.data_ringbuffer):
            assert chunk.chunk_id == i
            group.add_free_chunk(chunk)
