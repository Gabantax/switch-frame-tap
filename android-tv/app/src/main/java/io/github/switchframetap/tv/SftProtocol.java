/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import java.io.EOFException;
import java.io.IOException;
import java.util.Arrays;

/** SFTR v2, not the unrelated sw_hdr_t protocol under switch-stream/. */
final class SftProtocol {
    static final int VIDEO = 2, KEY = 4, AUDIO = 8, STATUS = 16;
    /** v0.7.16 consoles: the part of a video frame the game drew (in the header's
     *  block_h_log2) and whether it sits in the middle. A console sends them only
     *  to a viewer that says it draws them ("SFTC"); this app does not, so the
     *  console scales such frames itself. Accepted anyway, so a frame meant for
     *  another viewer never drops the connection. */
    static final int CROP = 32, CROP_MID = 64;
    static final int MAX_VIDEO = 8 * 1024 * 1024, MAX_AUDIO = 65536, MAX_STATUS = 4096;
    interface Stop { boolean stopped(); }
    interface Source extends AutoCloseable {
        /** Returns 0 on an idle timeout, -1 on EOF. Never discard partial reads. */
        int read(byte[] data, int offset, int length) throws IOException;
        @Override void close() throws IOException;
        /** "SFTK": the console sends a keyframe now instead of at its next one
         *  (up to 4 s away over the network). Consoles before v0.7.13 ignore it. */
        default void requestKeyframe() { }
    }
    static final byte[] KEYFRAME_REQUEST = { 'S', 'F', 'T', 'K' };
    static final class Packet {
        final int flags, width, height;
        final long sequence, consoleAgeUs, receivedNs;
        final byte[] data;
        boolean discontinuity;
        Packet(int flags, int width, int height, long sequence, long age, byte[] data, long receivedNs) {
            this.flags = flags; this.width = width; this.height = height;
            this.sequence = sequence; this.consoleAgeUs = age; this.data = data;
            this.receivedNs = receivedNs;
        }
        boolean key() { return (flags & KEY) != 0; }
        boolean audio() { return (flags & AUDIO) != 0 && (flags & STATUS) == 0; }
        /** The console's reason for showing no picture (Switch Frame Tap 0.7.10+). */
        boolean status() { return (flags & STATUS) != 0; }
        int statusReason() { return data.length >= 8 ? (int)u32(data, 4) : 0; }
        String statusHead() { return cString(data, 8, 64); }
        String statusDetail() { return cString(data, 72, 120); }
    }
    static Packet readPacket(Source source, Stop stopped) throws IOException {
        byte[] h = new byte[32];
        int flags;
        long w, height, n, receivedNs;
        while (true) {
            readFully(source, h, true, stopped);
            receivedNs = System.nanoTime();
            if (u32(h, 0) != 0x52544653L) throw new IOException("Invalid SFTR header");
            if (u16(h, 4) != 2) throw new IOException("SFTR protocol v2 is required");
            flags = u16(h, 6);
            w = u32(h, 8); height = u32(h, 12); n = u32(h, 20);
            // An empty header is the console checking that a viewer reads (USB): nothing follows.
            if (flags == 0 && n == 0) continue;
            break;
        }
        if ((flags & ~(VIDEO | KEY | AUDIO | STATUS | CROP | CROP_MID)) != 0) throw new IOException("Unsupported stream format");
        if ((flags & STATUS) != 0) {
            // Audio + status, rate 0: why there is no picture. Older viewers drop it as audio.
            if (flags != (AUDIO | STATUS) || n < 8 || n > MAX_STATUS) throw new IOException("Invalid status packet");
            byte[] data = new byte[(int)n];
            readFully(source, data, false, stopped);
            return new Packet(flags, 0, 0, u32(h, 28), u32(h, 16), data, receivedNs);
        }
        boolean audio = (flags & AUDIO) != 0;
        if (audio) {
            if (flags != AUDIO || w != 48000 || height != 2 || n == 0 || n > MAX_AUDIO || n % 4 != 0)
                throw new IOException("Invalid PCM packet: 48 kHz stereo, 16-bit audio is required");
        } else if ((flags & VIDEO) == 0 || w < 16 || w > 1920 || height < 16 || height > 1088
                || w % 2 != 0 || height % 2 != 0 || n == 0 || n > MAX_VIDEO) {
            throw new IOException("Invalid or oversized H.264 packet");
        }
        byte[] data = new byte[(int)n];
        readFully(source, data, false, stopped);
        return new Packet(flags, (int)w, (int)height, u32(h, 28), u32(h, 16), data, receivedNs);
    }
    private static void readFully(Source s, byte[] data, boolean idleAllowed, Stop stop) throws IOException {
        int done = 0;
        long deadline = idleAllowed ? Long.MAX_VALUE : System.nanoTime() + 5_000_000_000L;
        while (done < data.length) {
            if (stop.stopped()) throw new IOException("Connection stopped");
            if (System.nanoTime() > deadline) throw new IOException("Incomplete packet: read timed out");
            int n = s.read(data, done, data.length - done);
            if (n < 0) throw new EOFException("The Switch closed the connection");
            if (n > data.length - done) throw new IOException("Read exceeds buffer bounds");
            if (n > 0) {
                if (done == 0 && idleAllowed) deadline = System.nanoTime() + 5_000_000_000L;
                done += n;
            }
        }
    }
    static String cString(byte[] b, int offset, int max) {
        int end = offset;
        while (end < b.length && end < offset + max && b[end] != 0) end++;
        return offset >= b.length ? "" : new String(b, offset, end - offset, java.nio.charset.StandardCharsets.UTF_8);
    }
    static int u16(byte[] b, int i) { return (b[i] & 255) | ((b[i+1] & 255) << 8); }
    static long u32(byte[] b, int i) {
        return (b[i] & 255L) | ((b[i+1] & 255L) << 8) | ((b[i+2] & 255L) << 16) | ((b[i+3] & 255L) << 24);
    }
    /** Codec-specific buffers retain their Annex-B start codes. */
    static byte[][] parameterSets(byte[] data) throws IOException {
        byte[] sps = null, pps = null;
        for (int i = 0; i < data.length - 3;) {
            int prefix = prefix(data, i);
            if (prefix == 0) { i++; continue; }
            int start = i, nal = i + prefix, end = nal + 1;
            if (nal >= data.length) break;
            while (end < data.length && prefix(data, end) == 0) end++;
            int type = data[nal] & 31;
            if (type == 7) sps = Arrays.copyOfRange(data, start, end);
            if (type == 8) pps = Arrays.copyOfRange(data, start, end);
            i = end;
        }
        if (sps == null || pps == null) throw new IOException("The keyframe does not contain SPS/PPS");
        return new byte[][] {sps, pps};
    }
    /** v0.7.16: a frame for an MP4 sample - its picture NAL units only. SPS and
     *  PPS go in the track's format (parameterSets), and Android's muxer splits
     *  NAL units only near the start of a sample, so nothing else may lead. */
    static byte[] pictureNals(byte[] data) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(data.length);
        for (int i = 0; i < data.length - 3;) {
            int prefix = prefix(data, i);
            if (prefix == 0) { i++; continue; }
            int nal = i + prefix, end = nal + 1;
            if (nal >= data.length) break;
            while (end < data.length && prefix(data, end) == 0) end++;
            int type = data[nal] & 31;
            if (type >= 1 && type <= 5) { out.write(0); out.write(0); out.write(0); out.write(1); out.write(data, nal, end - nal); }
            i = end;
        }
        return out.toByteArray();
    }
    private static int prefix(byte[] b, int i) {
        if (i+2 >= b.length || b[i] != 0 || b[i+1] != 0) return 0;
        if (b[i+2] == 1) return 3;
        return i+3 < b.length && b[i+2] == 0 && b[i+3] == 1 ? 4 : 0;
    }
    static boolean ipv4(String value) {
        String[] parts = value.split("\\.", -1);
        if (parts.length != 4) return false;
        for (String p : parts) {
            if (p.isEmpty() || p.length() > 3) return false;
            for (int i = 0; i < p.length(); i++) if (p.charAt(i) < '0' || p.charAt(i) > '9') return false;
            if (Integer.parseInt(p) > 255) return false;
        }
        return true;
    }
    static int beaconPort(String message) {
        String[] p = message.trim().split("\\s+");
        if (p.length < 3 || !p[0].equals("SFTAP1") || !ipv4(p[1])) return -1;
        try { int port = Integer.parseInt(p[2]); return port > 0 && port <= 65535 ? port : -1; }
        catch (NumberFormatException e) { return -1; }
    }
}
