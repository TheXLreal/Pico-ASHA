# ASHA streaming model

Pico ASHA follows the Android ASHA data path for the mandatory G.722 16 kHz
codec:

- USB PCM is encoded into 20 ms frames: one sequence byte followed by 160 bytes
  of G.722 audio.
- A stream start creates a new encoder epoch. Both G.722 encoders, both 48 kHz
  decimators, the sequence number, and the local queue are reset before the
  first frame is published.
- The producer-to-Bluetooth queue contains exactly eight SDUs. If a receiver is
  more than eight frames behind, stale local frames are discarded and streaming
  continues from the newest complete frame; the Audio Control Point is not
  restarted merely because L2CAP credits are temporarily exhausted.
- Each hearing aid owns its in-flight SDU until BTstack reports that it was sent.
  Ring slots are copied under a cross-core lock, so wrapping the queue cannot
  change a packet while BTstack is using it.
- Binaural devices are paired by their HiSync ID and opposite left/right sides.
  Audio does not begin until the outstanding Start commands have received Audio
  Status responses. A remaining single device receives a mono mix.

The BLE setup requests a 20 ms connection interval, peripheral latency 10, and
supervision timeout 1 second. The L2CAP CoC uses a local MTU of 512 bytes and
rejects a remote MTU below 167 bytes. ASHA devices are expected to provide eight
initial credits; fewer credits are reported but are accepted for compatibility.

The Read Only Properties characteristic must be version 1, advertise LE CoC,
and support G.722 at 16 kHz. The ASHA service also requires readable ROP and PSM
characteristics, a writable Audio Control Point, a notifiable Audio Status
Point, and a write-without-response Volume characteristic.

