package com.phonecastvr.sender;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.ByteArrayInputStream;

public final class StreamProtocolTest {
    @Test public void writesVersionedBigEndianHeader() throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        StreamProtocol.write(new DataOutputStream(bytes), StreamProtocol.TYPE_VIDEO_FRAME,
                StreamProtocol.FLAG_KEY_FRAME, 42, 123456, 1080, 1920,
                new byte[]{1, 2, 3});
        byte[] message = bytes.toByteArray();
        assertEquals(35, message.length);
        assertEquals('P', message[0]);
        assertEquals('R', message[3]);
        assertEquals(StreamProtocol.VERSION, message[4]);
        assertEquals(StreamProtocol.TYPE_VIDEO_FRAME, message[5]);
        assertEquals(3, message[11]);
        assertEquals(1, message[32]);
    }

    @Test public void keyFrameRequestHasStableWireType() {
        assertEquals(7, StreamProtocol.TYPE_REQUEST_KEY_FRAME);
        assertEquals(8, StreamProtocol.TYPE_REMOTE_INPUT);
        assertEquals(9, StreamProtocol.TYPE_NOTIFICATION);
        assertEquals(10, StreamProtocol.TYPE_REMOTE_CONTROL_STATUS);
        assertEquals(11, StreamProtocol.TYPE_NOTIFICATION_OPEN);
    }

    @Test public void writesBoundedNotificationPayload() throws Exception {
        byte[] payload = StreamProtocol.notificationPayload(
                "Messages", "Hello", "Are you coming?", "com.example.messages",
                1234L, 99L, false);
        DataInputStream input = new DataInputStream(new ByteArrayInputStream(payload));
        assertEquals(2, input.readUnsignedByte());
        assertEquals(0, input.readUnsignedByte());
        assertEquals(8, input.readUnsignedShort());
        assertEquals(5, input.readUnsignedShort());
        assertEquals(15, input.readUnsignedShort());
        assertEquals(20, input.readUnsignedShort());
        assertEquals(1234L, input.readLong());
        assertEquals(99L, input.readLong());
    }

    @Test public void notificationPolicyUsesAllowAndBlockLists() {
        assertTrue(NotificationPolicy.allows("com.example.chat", "", ""));
        assertTrue(NotificationPolicy.allows("com.example.chat",
                "com.example.chat, com.example.mail", ""));
        assertFalse(NotificationPolicy.allows("com.example.bank",
                "com.example.chat", ""));
        assertFalse(NotificationPolicy.allows("com.example.chat",
                "com.example.chat", "com.example.chat"));
        assertEquals("hello world", NotificationPolicy.clean(" hello\n world ", 20));
    }

    @Test public void parsesNormalizedRemoteInput() throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream output = new DataOutputStream(bytes);
        output.writeByte(RemoteInputEvent.SCROLL);
        output.write(new byte[3]);
        output.writeFloat(0.25f);
        output.writeFloat(0.75f);
        output.writeFloat(-0.5f);
        RemoteInputEvent event = StreamProtocol.parseRemoteInput(bytes.toByteArray(), 91);
        assertEquals(RemoteInputEvent.SCROLL, event.type);
        assertEquals(0.25f, event.normalizedX, 0.0001f);
        assertEquals(0.75f, event.normalizedY, 0.0001f);
        assertEquals(-0.5f, event.scrollDelta, 0.0001f);
        assertEquals(91, event.sequence);
    }

    @Test public void writesIndependentRemoteControlGateStatus() {
        byte[] both = StreamProtocol.remoteControlStatusPayload(true, true);
        assertEquals(2, both.length);
        assertEquals(1, both[0]);
        assertEquals(3, both[1]);
        byte[] appOnly = StreamProtocol.remoteControlStatusPayload(true, false);
        assertEquals(1, appOnly[1]);
    }

    @Test public void validatesPairCodes() {
        assertTrue(StreamProtocol.validPairCode("123456"));
        assertFalse(StreamProtocol.validPairCode("12345"));
        assertFalse(StreamProtocol.validPairCode("12345x"));
    }

    @Test public void trackingWireTypesHaveStableValues() {
        assertEquals(15, StreamProtocol.TYPE_SCREEN_METRICS);
        assertEquals(16, StreamProtocol.TYPE_PHONE_IMU);
        assertEquals(15, TrackingProtocol.TYPE_SCREEN_METRICS);
        assertEquals(16, TrackingProtocol.TYPE_PHONE_IMU);
    }

    @Test public void writesBoundedScreenMetricsPayload() throws Exception {
        byte[] payload = StreamProtocol.screenMetricsPayload(420, 1080, 2400,
                68.5f, 152.2f, 400.2f, 400.5f, true);
        assertEquals(32, payload.length);
        assertEquals(1, payload[0]); // version
        assertEquals(1, payload[1]); // measured flags
        assertEquals(420, ((payload[2] & 0xFF) << 8) | (payload[3] & 0xFF));
        assertEquals(0, payload[28]);
        assertEquals(0, payload[29]);
        assertEquals(0, payload[30]);
        assertEquals(0, payload[31]);
    }

    @Test public void parsesScreenMetricsRoundTrip() throws Exception {
        byte[] payload = StreamProtocol.screenMetricsPayload(420, 1080, 2400,
                68.5f, 152.2f, 400.2f, 400.5f, true);
        StreamProtocol.ScreenMetrics metrics = StreamProtocol.parseScreenMetrics(payload);
        assertEquals(1, metrics.version);
        assertEquals(1, metrics.flags);
        assertEquals(420, metrics.densityDpi);
        assertEquals(1080, metrics.pixelWidth);
        assertEquals(2400, metrics.pixelHeight);
        assertEquals(68.5f, metrics.physicalWidthMm, 0.001f);
        assertEquals(152.2f, metrics.physicalHeightMm, 0.001f);
        assertEquals(400.2f, metrics.xdpi, 0.001f);
        assertEquals(400.5f, metrics.ydpi, 0.001f);
    }

    @Test(expected = java.io.IOException.class)
    public void screenMetricsRejectsInvalidLength() throws Exception {
        StreamProtocol.parseScreenMetrics(new byte[31]);
    }

    @Test(expected = java.io.IOException.class)
    public void screenMetricsRejectsUnsupportedVersion() throws Exception {
        byte[] payload = StreamProtocol.screenMetricsPayload(420, 1080, 2400,
                68.5f, 152.2f, 400.2f, 400.5f, true);
        payload[0] = 2;
        StreamProtocol.parseScreenMetrics(payload);
    }

    @Test(expected = java.io.IOException.class)
    public void screenMetricsRejectsReservedFlags() throws Exception {
        byte[] payload = StreamProtocol.screenMetricsPayload(420, 1080, 2400,
                68.5f, 152.2f, 400.2f, 400.5f, true);
        payload[1] = 0x02;
        StreamProtocol.parseScreenMetrics(payload);
    }

    @Test(expected = java.io.IOException.class)
    public void screenMetricsRejectsNonZeroReservedBytes() throws Exception {
        byte[] payload = StreamProtocol.screenMetricsPayload(420, 1080, 2400,
                68.5f, 152.2f, 400.2f, 400.5f, true);
        payload[28] = 1;
        StreamProtocol.parseScreenMetrics(payload);
    }

    @Test public void serializesZeroAllocationPhoneImu() throws Exception {
        byte[] buffer = new byte[48];
        StreamProtocol.serializePhoneImuPayload(buffer, 1, 3, 0,
                0.0f, 0.0f, 0.0f, 1.0f,
                0.1f, 0.2f, 0.3f,
                0.4f, 0.5f, 0.6f);
        StreamProtocol.PhoneImu imu = StreamProtocol.parsePhoneImu(buffer);
        assertEquals(1, imu.version);
        assertEquals(1, imu.sensorType);
        assertEquals(3, imu.accuracy);
        assertEquals(0, imu.flags);
        assertEquals(0.0f, imu.qx, 0.0001f);
        assertEquals(0.0f, imu.qy, 0.0001f);
        assertEquals(0.0f, imu.qz, 0.0001f);
        assertEquals(1.0f, imu.qw, 0.0001f);
        assertEquals(0.1f, imu.gyroX, 0.0001f);
        assertEquals(0.2f, imu.gyroY, 0.0001f);
        assertEquals(0.3f, imu.gyroZ, 0.0001f);
        assertEquals(0.4f, imu.accelX, 0.0001f);
        assertEquals(0.5f, imu.accelY, 0.0001f);
        assertEquals(0.6f, imu.accelZ, 0.0001f);
    }

    @Test public void parsesPhoneImuRoundTrip() throws Exception {
        byte[] payload = StreamProtocol.phoneImuPayload(2, 2, 3,
                0.5f, 0.5f, 0.5f, 0.5f,
                1.5f, -2.5f, 3.5f,
                0.0f, 9.8f, -9.8f);
        StreamProtocol.PhoneImu imu = StreamProtocol.parsePhoneImu(payload);
        assertEquals(1, imu.version);
        assertEquals(2, imu.sensorType);
        assertEquals(2, imu.accuracy);
        assertEquals(3, imu.flags);
        assertEquals(0.5f, imu.qx, 0.0001f);
        assertEquals(0.5f, imu.qy, 0.0001f);
        assertEquals(0.5f, imu.qz, 0.0001f);
        assertEquals(0.5f, imu.qw, 0.0001f);
        assertEquals(1.5f, imu.gyroX, 0.0001f);
        assertEquals(-2.5f, imu.gyroY, 0.0001f);
        assertEquals(3.5f, imu.gyroZ, 0.0001f);
        assertEquals(0.0f, imu.accelX, 0.0001f);
        assertEquals(9.8f, imu.accelY, 0.0001f);
        assertEquals(-9.8f, imu.accelZ, 0.0001f);
    }

    @Test(expected = java.io.IOException.class)
    public void phoneImuRejectsUnnormalizedQuaternion() throws Exception {
        byte[] payload = StreamProtocol.phoneImuPayload(1, 3, 0,
                0.0f, 0.0f, 0.0f, 0.0f,
                0f, 0f, 0f, 0f, 0f, 0f);
        StreamProtocol.parsePhoneImu(payload);
    }

    @Test(expected = java.io.IOException.class)
    public void phoneImuRejectsExtremeAngularVelocity() throws Exception {
        byte[] payload = StreamProtocol.phoneImuPayload(1, 3, 0,
                0.0f, 0.0f, 0.0f, 1.0f,
                55.0f, 0f, 0f, 0f, 0f, 0f);
        StreamProtocol.parsePhoneImu(payload);
    }

    @Test(expected = java.io.IOException.class)
    public void phoneImuRejectsExtremeAcceleration() throws Exception {
        byte[] payload = StreamProtocol.phoneImuPayload(1, 3, 0,
                0.0f, 0.0f, 0.0f, 1.0f,
                0f, 0f, 0f, 0f, 0f, 250.0f);
        StreamProtocol.parsePhoneImu(payload);
    }

    @Test(expected = java.io.IOException.class)
    public void phoneImuRejectsNonZeroReservedBytes() throws Exception {
        byte[] payload = StreamProtocol.phoneImuPayload(1, 3, 0,
                0.0f, 0.0f, 0.0f, 1.0f,
                0f, 0f, 0f, 0f, 0f, 0f);
        payload[44] = 1;
        StreamProtocol.parsePhoneImu(payload);
    }

    @Test public void trackingCapabilityTokenMatches() {
        byte[] token = TrackingProtocol.capability();
        assertEquals(8, token.length);
        assertEquals('P', token[0]);
        assertEquals('C', token[1]);
        assertEquals('T', token[2]);
        assertEquals('R', token[3]);
        assertEquals(1, token[4]);
        assertEquals(1, token[5]);
        assertEquals(0, token[6]);
        assertEquals(0, token[7]);

        assertTrue(TrackingProtocol.hasCapability(token));
        assertFalse(TrackingProtocol.hasCapability(new byte[7]));
        assertFalse(TrackingProtocol.hasCapability(new byte[]{1, 2, 3, 4, 5, 6, 7, 8}));

        byte[] combined = TrackingProtocol.combineCapabilities(true, true);
        assertEquals(16, combined.length);
        assertTrue(AudioProtocol.hasCapability(java.util.Arrays.copyOfRange(combined, 0, 8)));
        assertTrue(TrackingProtocol.hasCapability(combined));

        byte[] trackingOnly = TrackingProtocol.combineCapabilities(false, true);
        assertEquals(8, trackingOnly.length);
        assertTrue(TrackingProtocol.hasCapability(trackingOnly));

        byte[] neither = TrackingProtocol.combineCapabilities(false, false);
        assertEquals(0, neither.length);
    }

    @Test public void testPhoneImuZeroAllocationHotPath() throws Exception {
        byte[] buffer = new byte[48];
        // Warm up JIT compiler
        for (int i = 0; i < 20_000; i++) {
            StreamProtocol.serializePhoneImuPayload(buffer, 1, 3, 0,
                    0.0f, 0.0f, 0.0f, 1.0f,
                    0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f);
        }

        try {
            Class<?> factoryClass = Class.forName("java.lang.management.ManagementFactory");
            Object bean = factoryClass.getMethod("getThreadMXBean").invoke(null);
            Class<?> mxBeanClass = Class.forName("com.sun.management.ThreadMXBean");
            java.lang.reflect.Method getBytesMethod = mxBeanClass.getMethod("getThreadAllocatedBytes", long.class);
            Long tidObj = Thread.currentThread().getId();
            Object[] args = new Object[]{ tidObj };

            // Warm up reflection invocation to trigger bytecode accessor inflation
            for (int i = 0; i < 50; i++) {
                getBytesMethod.invoke(bean, args);
            }

            // Baseline overhead of the reflection call itself
            long b1 = (Long) getBytesMethod.invoke(bean, args);
            long b2 = (Long) getBytesMethod.invoke(bean, args);
            long reflectionOverhead = b2 - b1;

            long before = (Long) getBytesMethod.invoke(bean, args);
            for (int i = 0; i < 100_000; i++) {
                StreamProtocol.serializePhoneImuPayload(buffer, 1, 3, 0,
                        0.0f, 0.0f, 0.0f, 1.0f,
                        0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f);
            }
            long after = (Long) getBytesMethod.invoke(bean, args);
            long netAllocated = (after - before) - reflectionOverhead;
            // Hot path must not allocate any heap objects per iteration (smallest JVM object is 16B)
            assertTrue("Expected zero allocations on phone IMU serialization hot path, total net bytes=" + netAllocated,
                    netAllocated < 1024 && (netAllocated / 100_000) == 0L);
        } catch (ClassNotFoundException | NoSuchMethodException ignored) {
            // JVM does not support ThreadMXBean; skip allocation assertion
        }
    }

    @Test public void testNetworkStreamerImuPoolZeroAllocation() throws Exception {
        NetworkStreamer streamer = new NetworkStreamer("127.0.0.1", 49321, "123456", new NetworkStreamer.Listener() {
            @Override public void onConnectionChanged(boolean connected, String message) {}
            @Override public void onKeyFrameNeeded() {}
            @Override public void onRemoteInput(RemoteInputEvent event) {}
            @Override public void onNotificationOpen(long actionToken) {}
            @Override public void onAudioCapability(boolean supported) {}
        });

        java.lang.reflect.Field trackingField = NetworkStreamer.class.getDeclaredField("trackingSupported");
        trackingField.setAccessible(true);
        trackingField.setBoolean(streamer, true);

        java.lang.reflect.Field runningField = NetworkStreamer.class.getDeclaredField("running");
        runningField.setAccessible(true);
        ((java.util.concurrent.atomic.AtomicBoolean) runningField.get(streamer)).set(true);

        java.lang.reflect.Field imuFramesField = NetworkStreamer.class.getDeclaredField("imuFrames");
        imuFramesField.setAccessible(true);
        java.util.concurrent.ArrayBlockingQueue<?> imuFrames = (java.util.concurrent.ArrayBlockingQueue<?>) imuFramesField.get(streamer);

        java.lang.reflect.Field imuPoolField = NetworkStreamer.class.getDeclaredField("imuPool");
        imuPoolField.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.concurrent.ArrayBlockingQueue<Object> imuPool = (java.util.concurrent.ArrayBlockingQueue<Object>) imuPoolField.get(streamer);

        // Warm up JIT and verify steady-state recycling
        for (int i = 0; i < 5000; i++) {
            streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
            Object packet = imuFrames.poll();
            if (packet != null) imuPool.offer(packet);
        }

        try {
            Class<?> factoryClass = Class.forName("java.lang.management.ManagementFactory");
            Object bean = factoryClass.getMethod("getThreadMXBean").invoke(null);
            Class<?> mxBeanClass = Class.forName("com.sun.management.ThreadMXBean");
            java.lang.reflect.Method getBytesMethod = mxBeanClass.getMethod("getThreadAllocatedBytes", long.class);
            Long tidObj = Thread.currentThread().getId();
            Object[] args = new Object[]{ tidObj };

            // Warm up reflection invocation to trigger bytecode accessor inflation
            for (int i = 0; i < 50; i++) {
                getBytesMethod.invoke(bean, args);
            }

            long b1 = (Long) getBytesMethod.invoke(bean, args);
            long b2 = (Long) getBytesMethod.invoke(bean, args);
            long reflectionOverhead = b2 - b1;

            long before = (Long) getBytesMethod.invoke(bean, args);
            for (int i = 0; i < 50_000; i++) {
                streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
                Object packet = imuFrames.poll();
                if (packet != null) imuPool.offer(packet);
            }
            long after = (Long) getBytesMethod.invoke(bean, args);
            long netAllocated = (after - before) - reflectionOverhead;
            assertTrue("Expected zero allocations on NetworkStreamer.offerImu with pooled packets, total net bytes=" + netAllocated,
                    netAllocated < 1024 && (netAllocated / 50_000) == 0L);
        } catch (ClassNotFoundException | NoSuchMethodException ignored) {
            // JVM does not support ThreadMXBean; skip allocation assertion
        }
    }

    @Test public void testNetworkStreamerDropOldestZeroAllocationUnderStall() throws Exception {
        NetworkStreamer streamer = new NetworkStreamer("127.0.0.1", 49321, "123456", new NetworkStreamer.Listener() {
            @Override public void onConnectionChanged(boolean connected, String message) {}
            @Override public void onKeyFrameNeeded() {}
            @Override public void onRemoteInput(RemoteInputEvent event) {}
            @Override public void onNotificationOpen(long actionToken) {}
            @Override public void onAudioCapability(boolean supported) {}
        });

        java.lang.reflect.Field trackingField = NetworkStreamer.class.getDeclaredField("trackingSupported");
        trackingField.setAccessible(true);
        trackingField.setBoolean(streamer, true);

        java.lang.reflect.Field runningField = NetworkStreamer.class.getDeclaredField("running");
        runningField.setAccessible(true);
        ((java.util.concurrent.atomic.AtomicBoolean) runningField.get(streamer)).set(true);

        java.lang.reflect.Field imuFramesField = NetworkStreamer.class.getDeclaredField("imuFrames");
        imuFramesField.setAccessible(true);
        java.util.concurrent.ArrayBlockingQueue<?> imuFrames = (java.util.concurrent.ArrayBlockingQueue<?>) imuFramesField.get(streamer);

        // Fill up imuFrames and drain pool to simulate persistent network stall
        for (int i = 0; i < 30; i++) {
            streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
        }
        assertEquals(16, imuFrames.size());

        // Warm up drop-oldest branch and let C2 JIT optimize
        for (int i = 0; i < 100_000; i++) {
            streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
        }
        assertEquals(16, imuFrames.size());

        try {
            Class<?> factoryClass = Class.forName("java.lang.management.ManagementFactory");
            Object bean = factoryClass.getMethod("getThreadMXBean").invoke(null);
            Class<?> mxBeanClass = Class.forName("com.sun.management.ThreadMXBean");
            java.lang.reflect.Method getBytesMethod = mxBeanClass.getMethod("getThreadAllocatedBytes", long.class);
            Long tidObj = Thread.currentThread().getId();
            Object[] args = new Object[]{ tidObj };

            // Warm up reflection invocation to trigger bytecode accessor inflation
            for (int i = 0; i < 50; i++) {
                getBytesMethod.invoke(bean, args);
            }

            long b1 = (Long) getBytesMethod.invoke(bean, args);
            long b2 = (Long) getBytesMethod.invoke(bean, args);
            long reflectionOverhead = b2 - b1;

            long start10 = (Long) getBytesMethod.invoke(bean, args);
            for (int i = 0; i < 10; i++) {
                streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
            }
            long end10 = (Long) getBytesMethod.invoke(bean, args);
            long alloc10 = (end10 - start10) - reflectionOverhead;

            long start10k = (Long) getBytesMethod.invoke(bean, args);
            for (int i = 0; i < 50_000; i++) {
                streamer.offerImu(1000L, 1, 3, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 0f, 0f);
            }
            long end10k = (Long) getBytesMethod.invoke(bean, args);
            long alloc10k = (end10k - start10k) - reflectionOverhead;

            assertTrue("Zero allocations expected on NetworkStreamer.offerImu even during drop-oldest stall, total net bytes=" + alloc10k,
                    alloc10k < 1024 && (alloc10k / 50_000) == 0L);
        } catch (ClassNotFoundException | NoSuchMethodException ignored) {
            // JVM does not support ThreadMXBean; skip allocation assertion
        }
        assertEquals(16, imuFrames.size());
    }
}

