package com.phonecastvr.sender;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

final class StreamProtocol {
    static final int VERSION = 1;
    static final int DEFAULT_PORT = 49321;
    static final int TYPE_HELLO = 1;
    static final int TYPE_VIDEO_CONFIG = 2;
    static final int TYPE_VIDEO_FRAME = 3;
    static final int TYPE_PING = 4;
    static final int TYPE_PONG = 5;
    static final int TYPE_END_STREAM = 6;
    static final int TYPE_REQUEST_KEY_FRAME = 7;
    static final int TYPE_REMOTE_INPUT = 8;
    static final int TYPE_NOTIFICATION = 9;
    static final int TYPE_REMOTE_CONTROL_STATUS = 10;
    static final int TYPE_NOTIFICATION_OPEN = 11;
    static final int TYPE_SCREEN_METRICS = 15;
    static final int TYPE_PHONE_IMU = 16;
    static final int TYPE_REMOTE_TEXT = 17;
    static final int FLAG_KEY_FRAME = 1;
    static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;

    private StreamProtocol() {}

    static void write(DataOutputStream output, int type, int flags, long sequence,
                      long timestampMicros, int width, int height, byte[] payload)
            throws IOException {
        if (payload == null) payload = new byte[0];
        if (payload.length > MAX_PAYLOAD_SIZE) {
            throw new IOException("Protocol payload exceeds 4 MiB");
        }
        output.writeByte('P');
        output.writeByte('C');
        output.writeByte('V');
        output.writeByte('R');
        output.writeByte(VERSION);
        output.writeByte(type);
        output.writeShort(flags);
        output.writeInt(payload.length);
        output.writeLong(sequence);
        output.writeLong(timestampMicros);
        output.writeShort(width);
        output.writeShort(height);
        output.write(payload);
    }

    static Header readHeader(DataInputStream input) throws IOException {
        if (input.readUnsignedByte() != 'P' || input.readUnsignedByte() != 'C' ||
                input.readUnsignedByte() != 'V' || input.readUnsignedByte() != 'R') {
            throw new IOException("Invalid PhoneCast protocol magic");
        }
        int version = input.readUnsignedByte();
        if (version != VERSION) throw new IOException("Unsupported PhoneCast protocol version");
        int type = input.readUnsignedByte();
        int flags = input.readUnsignedShort();
        int payloadSize = input.readInt();
        if (payloadSize < 0 || payloadSize > MAX_PAYLOAD_SIZE) {
            throw new IOException("Invalid PhoneCast payload size");
        }
        long sequence = input.readLong();
        long timestampMicros = input.readLong();
        int width = input.readUnsignedShort();
        int height = input.readUnsignedShort();
        return new Header(type, flags, payloadSize, sequence, timestampMicros, width, height);
    }

    static byte[] notificationPayload(String applicationName, String title, String body,
                                      String packageName, long postedAtMillis,
                                      long actionToken, boolean contentRedacted) throws IOException {
        byte[] application = utf8Field(applicationName);
        byte[] heading = utf8Field(title);
        byte[] text = utf8Field(body);
        byte[] source = utf8Field(packageName);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(
                26 + application.length + heading.length + text.length + source.length);
        DataOutputStream output = new DataOutputStream(bytes);
        output.writeByte(2);
        output.writeByte(contentRedacted ? 1 : 0);
        output.writeShort(application.length);
        output.writeShort(heading.length);
        output.writeShort(text.length);
        output.writeShort(source.length);
        output.writeLong(postedAtMillis);
        output.writeLong(actionToken);
        output.write(application);
        output.write(heading);
        output.write(text);
        output.write(source);
        return bytes.toByteArray();
    }

    private static byte[] utf8Field(String value) throws IOException {
        byte[] bytes = (value == null ? "" : value).getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (bytes.length > 1024) throw new IOException("Notification field exceeds 1024 bytes");
        return bytes;
    }

    static byte[] remoteControlStatusPayload(boolean appEnabled,
                                             boolean accessibilityEnabled) {
        int flags = (appEnabled ? 1 : 0) | (accessibilityEnabled ? 2 : 0);
        return new byte[]{1, (byte) flags};
    }

    static RemoteInputEvent parseRemoteInput(byte[] payload, long sequence) throws IOException {
        if (payload == null || payload.length != 16) {
            throw new IOException("Remote-input payload must be exactly 16 bytes");
        }
        DataInputStream input = new DataInputStream(new ByteArrayInputStream(payload));
        int type = input.readUnsignedByte();
        input.skipBytes(3);
        float x = input.readFloat();
        float y = input.readFloat();
        float scroll = input.readFloat();
        if (type < RemoteInputEvent.DOWN || type > RemoteInputEvent.RECENTS ||
                !Float.isFinite(x) || !Float.isFinite(y) || !Float.isFinite(scroll) ||
                x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f ||
                scroll < -1.0f || scroll > 1.0f) {
            throw new IOException("Invalid normalized remote-input event");
        }
        return new RemoteInputEvent(type, x, y, scroll, sequence);
    }

    static byte[] remoteTextPayload(String text) throws IOException {
        byte[] bytes = (text == null ? "" : text).getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (bytes.length > 4096) throw new IOException("Remote text exceeds 4096 bytes");
        ByteArrayOutputStream buffer = new ByteArrayOutputStream(4 + bytes.length);
        DataOutputStream output = new DataOutputStream(buffer);
        output.writeByte(1); // version
        output.writeByte(0); // flags
        output.writeShort(bytes.length);
        output.write(bytes);
        return buffer.toByteArray();
    }

    static String parseRemoteText(byte[] payload) throws IOException {
        if (payload == null || payload.length < 4) {
            throw new IOException("Remote text payload is incomplete");
        }
        DataInputStream input = new DataInputStream(new ByteArrayInputStream(payload));
        int version = input.readUnsignedByte();
        if (version != 1) throw new IOException("Unsupported remote text version: " + version);
        int flags = input.readUnsignedByte();
        int length = input.readUnsignedShort();
        if (length > 4096 || length != payload.length - 4) {
            throw new IOException("Invalid remote text length: " + length);
        }
        byte[] textBytes = new byte[length];
        input.readFully(textBytes);
        return new String(textBytes, java.nio.charset.StandardCharsets.UTF_8);
    }

    static boolean validPairCode(String value) {
        return value != null && value.matches("[0-9]{6}");
    }

    static final class Header {
        final int type;
        final int flags;
        final int payloadSize;
        final long sequence;
        final long timestampMicros;
        final int width;
        final int height;

        Header(int type, int flags, int payloadSize, long sequence, long timestampMicros,
               int width, int height) {
            this.type = type;
            this.flags = flags;
            this.payloadSize = payloadSize;
            this.sequence = sequence;
            this.timestampMicros = timestampMicros;
            this.width = width;
            this.height = height;
        }
    }

    static final class ScreenMetrics {
        final int version;
        final int flags;
        final int densityDpi;
        final int pixelWidth;
        final int pixelHeight;
        final float physicalWidthMm;
        final float physicalHeightMm;
        final float xdpi;
        final float ydpi;

        ScreenMetrics(int version, int flags, int densityDpi, int pixelWidth, int pixelHeight,
                      float physicalWidthMm, float physicalHeightMm, float xdpi, float ydpi) {
            this.version = version;
            this.flags = flags;
            this.densityDpi = densityDpi;
            this.pixelWidth = pixelWidth;
            this.pixelHeight = pixelHeight;
            this.physicalWidthMm = physicalWidthMm;
            this.physicalHeightMm = physicalHeightMm;
            this.xdpi = xdpi;
            this.ydpi = ydpi;
        }
    }

    static final class PhoneImu {
        final int version;
        final int sensorType;
        final int accuracy;
        final int flags;
        final float qx;
        final float qy;
        final float qz;
        final float qw;
        final float gyroX;
        final float gyroY;
        final float gyroZ;
        final float accelX;
        final float accelY;
        final float accelZ;

        PhoneImu(int version, int sensorType, int accuracy, int flags,
                 float qx, float qy, float qz, float qw,
                 float gyroX, float gyroY, float gyroZ,
                 float accelX, float accelY, float accelZ) {
            this.version = version;
            this.sensorType = sensorType;
            this.accuracy = accuracy;
            this.flags = flags;
            this.qx = qx;
            this.qy = qy;
            this.qz = qz;
            this.qw = qw;
            this.gyroX = gyroX;
            this.gyroY = gyroY;
            this.gyroZ = gyroZ;
            this.accelX = accelX;
            this.accelY = accelY;
            this.accelZ = accelZ;
        }
    }

    static byte[] screenMetricsPayload(int densityDpi, int pixelWidth, int pixelHeight,
                                       float physicalWidthMm, float physicalHeightMm,
                                       float xdpi, float ydpi, boolean measuredDpi) throws IOException {
        if (pixelWidth <= 0 || pixelWidth > 16384 ||
            pixelHeight <= 0 || pixelHeight > 16384) {
            throw new IOException("Screen metrics pixel dimensions out of range");
        }
        if (!Float.isFinite(physicalWidthMm) || physicalWidthMm < 10.0f || physicalWidthMm > 1000.0f ||
            !Float.isFinite(physicalHeightMm) || physicalHeightMm < 10.0f || physicalHeightMm > 1000.0f) {
            throw new IOException("Screen metrics physical dimensions out of range");
        }
        if (!Float.isFinite(xdpi) || xdpi < 10.0f || xdpi > 2000.0f ||
            !Float.isFinite(ydpi) || ydpi < 10.0f || ydpi > 2000.0f) {
            throw new IOException("Screen metrics DPI out of range");
        }
        if (densityDpi <= 0 || densityDpi > 2000) {
            throw new IOException("Screen metrics density DPI out of range");
        }
        byte[] bytes = new byte[32];
        bytes[0] = 1;
        bytes[1] = (byte) (measuredDpi ? 1 : 0);
        bytes[2] = (byte) (densityDpi >> 8);
        bytes[3] = (byte) (densityDpi);
        writeInt(bytes, 4, pixelWidth);
        writeInt(bytes, 8, pixelHeight);
        writeFloat(bytes, 12, physicalWidthMm);
        writeFloat(bytes, 16, physicalHeightMm);
        writeFloat(bytes, 20, xdpi);
        writeFloat(bytes, 24, ydpi);
        return bytes;
    }

    static ScreenMetrics parseScreenMetrics(byte[] payload) throws IOException {
        if (payload == null || payload.length != 32) {
            throw new IOException("Screen metrics payload must be exactly 32 bytes");
        }
        if (payload[0] != 1) {
            throw new IOException("Unsupported screen metrics payload version: " + (payload[0] & 0xFF));
        }
        int flags = payload[1] & 0xFF;
        if ((flags & 0xFE) != 0) {
            throw new IOException("Reserved screen metrics flags must be zero");
        }
        int densityDpi = ((payload[2] & 0xFF) << 8) | (payload[3] & 0xFF);
        int pixelWidth = readInt(payload, 4);
        int pixelHeight = readInt(payload, 8);
        float physicalWidthMm = readFloat(payload, 12);
        float physicalHeightMm = readFloat(payload, 16);
        float xdpi = readFloat(payload, 20);
        float ydpi = readFloat(payload, 24);

        if (pixelWidth <= 0 || pixelWidth > 16384 ||
            pixelHeight <= 0 || pixelHeight > 16384) {
            throw new IOException("Screen metrics pixel dimensions out of range");
        }
        if (!Float.isFinite(physicalWidthMm) || physicalWidthMm < 10.0f || physicalWidthMm > 1000.0f ||
            !Float.isFinite(physicalHeightMm) || physicalHeightMm < 10.0f || physicalHeightMm > 1000.0f) {
            throw new IOException("Physical screen dimensions mm out of range");
        }
        if (!Float.isFinite(xdpi) || xdpi < 10.0f || xdpi > 2000.0f ||
            !Float.isFinite(ydpi) || ydpi < 10.0f || ydpi > 2000.0f) {
            throw new IOException("Screen metrics DPI values out of range");
        }
        if (densityDpi <= 0 || densityDpi > 2000) {
            throw new IOException("Screen metrics density DPI out of range");
        }
        if (payload[28] != 0 || payload[29] != 0 || payload[30] != 0 || payload[31] != 0) {
            throw new IOException("Reserved screen metrics bytes must be zero");
        }
        return new ScreenMetrics(payload[0] & 0xFF, flags, densityDpi, pixelWidth, pixelHeight,
                physicalWidthMm, physicalHeightMm, xdpi, ydpi);
    }

    static void serializePhoneImuPayload(byte[] target, int sensorType, int accuracy, int flags,
                                         float qx, float qy, float qz, float qw,
                                         float gyroX, float gyroY, float gyroZ,
                                         float accelX, float accelY, float accelZ) {
        target[0] = 1;
        target[1] = (byte) sensorType;
        target[2] = (byte) accuracy;
        target[3] = (byte) flags;
        writeFloat(target, 4, qx);
        writeFloat(target, 8, qy);
        writeFloat(target, 12, qz);
        writeFloat(target, 16, qw);
        writeFloat(target, 20, gyroX);
        writeFloat(target, 24, gyroY);
        writeFloat(target, 28, gyroZ);
        writeFloat(target, 32, accelX);
        writeFloat(target, 36, accelY);
        writeFloat(target, 40, accelZ);
        target[44] = 0;
        target[45] = 0;
        target[46] = 0;
        target[47] = 0;
    }

    static byte[] phoneImuPayload(int sensorType, int accuracy, int flags,
                                  float qx, float qy, float qz, float qw,
                                  float gyroX, float gyroY, float gyroZ,
                                  float accelX, float accelY, float accelZ) {
        byte[] bytes = new byte[48];
        serializePhoneImuPayload(bytes, sensorType, accuracy, flags,
                qx, qy, qz, qw, gyroX, gyroY, gyroZ, accelX, accelY, accelZ);
        return bytes;
    }

    static PhoneImu parsePhoneImu(byte[] payload) throws IOException {
        if (payload == null || payload.length != 48) {
            throw new IOException("Phone IMU payload must be exactly 48 bytes");
        }
        if (payload[0] != 1) {
            throw new IOException("Unsupported phone IMU payload version: " + (payload[0] & 0xFF));
        }
        int sensorType = payload[1] & 0xFF;
        if (sensorType != 1 && sensorType != 2) {
            throw new IOException("Unsupported sensor type");
        }
        int accuracy = payload[2] & 0xFF;
        if (accuracy > 3) {
            throw new IOException("Invalid sensor accuracy level");
        }
        int flags = payload[3] & 0xFF;
        if ((flags & 0xFC) != 0) {
            throw new IOException("Reserved phone IMU flags must be zero");
        }
        float qx = readFloat(payload, 4);
        float qy = readFloat(payload, 8);
        float qz = readFloat(payload, 12);
        float qw = readFloat(payload, 16);
        if (!Float.isFinite(qx) || !Float.isFinite(qy) || !Float.isFinite(qz) || !Float.isFinite(qw)) {
            throw new IOException("Phone IMU quaternion components must be finite");
        }
        float normSq = qx * qx + qy * qy + qz * qz + qw * qw;
        if (Math.abs(normSq - 1.0f) > 0.05f) {
            throw new IOException("Orientation quaternion is not normalized");
        }
        float gyroX = readFloat(payload, 20);
        float gyroY = readFloat(payload, 24);
        float gyroZ = readFloat(payload, 28);
        float accelX = readFloat(payload, 32);
        float accelY = readFloat(payload, 36);
        float accelZ = readFloat(payload, 40);
        if (!Float.isFinite(gyroX) || !Float.isFinite(gyroY) || !Float.isFinite(gyroZ) ||
            !Float.isFinite(accelX) || !Float.isFinite(accelY) || !Float.isFinite(accelZ)) {
            throw new IOException("Phone IMU sensor vectors must be finite");
        }
        if (Math.abs(gyroX) > 50.0f || Math.abs(gyroY) > 50.0f || Math.abs(gyroZ) > 50.0f) {
            throw new IOException("Angular velocity exceeds physical limit");
        }
        if (Math.abs(accelX) > 200.0f || Math.abs(accelY) > 200.0f || Math.abs(accelZ) > 200.0f) {
            throw new IOException("Acceleration exceeds physical limit");
        }
        if (payload[44] != 0 || payload[45] != 0 || payload[46] != 0 || payload[47] != 0) {
            throw new IOException("Reserved phone IMU bytes must be zero");
        }
        return new PhoneImu(payload[0] & 0xFF, sensorType, accuracy, flags,
                qx, qy, qz, qw, gyroX, gyroY, gyroZ, accelX, accelY, accelZ);
    }

    private static void writeInt(byte[] bytes, int offset, int value) {
        bytes[offset] = (byte) (value >> 24);
        bytes[offset + 1] = (byte) (value >> 16);
        bytes[offset + 2] = (byte) (value >> 8);
        bytes[offset + 3] = (byte) (value);
    }

    private static int readInt(byte[] bytes, int offset) {
        return ((bytes[offset] & 0xFF) << 24) |
               ((bytes[offset + 1] & 0xFF) << 16) |
               ((bytes[offset + 2] & 0xFF) << 8) |
               (bytes[offset + 3] & 0xFF);
    }

    private static void writeFloat(byte[] bytes, int offset, float value) {
        writeInt(bytes, offset, Float.floatToRawIntBits(value));
    }

    private static float readFloat(byte[] bytes, int offset) {
        return Float.intBitsToFloat(readInt(bytes, offset));
    }
}
