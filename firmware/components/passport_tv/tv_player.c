// Standalone FAV1 LAN prototype. Session workers own socket, audio and raw LCD.
#include "passport_tv.h"
#include "av_protocol.h"
#include "av_frame_buffer.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "bsp_battery.h"
#include "ui_menu.h"
#include "ui_menu_overlay.h"
#include "av_settings.h"
#include "nvs.h"
#include "av_rx_timing.h"
#include "av_channel_policy.h"
#include "ui_text.h"
#include "ui_status.h"
// The ROM's copy of miniz: tinfl_decompress lives at a fixed address in the
// ESP32-C3 mask ROM (see components/esp_rom/esp32c3/ld/esp32c3.rom.ld), so
// inflating costs no flash and no third-party component. Only the low-level
// entry point is used; the mem_to_mem convenience wrapper puts an 11 KB
// tinfl_decompressor on the caller's stack, and this task has four kilobytes.
#include "miniz.h"
// The compressed stripes the hardware benchmark inflates, generated on the host
// because the ROM's tdefl cannot run here. Included unconditionally: it is
// inert data and the compiler drops it when the benchmark is off, so guarding
// the include would only add a second place that knows the option.
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
// esp_cpu_get_cycle_count(), used only by the hardware benchmark to turn its
// work into cycles per byte and cycles per pixel. Included unconditionally
// because the header is free and a conditional include would be a second place
// that knows which option is on.
#include "esp_cpu.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/stats.h"
// Separate from sockets.h on purpose: name resolution is its own header in
// lwIP, and without it the address is dialled by number only.
#include "lwip/netdb.h"
// The USB peripheral, as a second transport for the media. See the note at
// AV_TRANSPORT_USB: the same FAV1 packets over a cable, at several times the
// rate the WiFi path manages.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define STOP BIT0
#define RX_DONE BIT1
#define AUDIO_DONE BIT2
#define VIDEO_DONE BIT3
#define CONFIG_READY BIT4
#define CLOCK_READY BIT5
#define WIFI_READY BIT6
// Six hundred milliseconds of sound, three times the I2S depth.
//
// The receiver reads one socket for both media, so while a video packet is
// being read the sound is not being read either. At the original 400 ms the
// queue ran dry during an ordinary frame and the session tore itself down.
// Two seconds fixed that but cost 32 KB of internal memory, which is the same
// memory the video buffers need: measured, the session was then left with

// The audio flow-control below uses PCM_QUEUE-4 as its ceiling, so this also
// sets how far the server may run ahead -- and that is what the depth is for.
//
// Thirty was too few, and the measurement that says so is the one this queue
// exists to prevent. The picture's packets are read by the same task that reads
// the sound, and while a picture packet is crossing, nothing drains this queue.
// With the sender's depth at 360 ms the queue had 160 ms of slack above it and
// the device calls an underrun after 300 ms of silence -- so any picture packet
// that took longer than 160 ms to read took the session down with it. Measured:
// every underrun was preceded by a ten-second interval with the sound *full*
// (499 to 502 packets against 500), which is what a timing fault looks like
// rather than a bandwidth one, and the gap recorded at the failure was 305 ms
// every time -- the threshold, not a number that varied.
//
// Twenty-four chunks is 960 ms, and its flow-control line (PCM_QUEUE-4) is 20
// chunks -- 800 ms. That headroom is the point: the sender leads the device by
// AUDIO_LEAD_MS, the device stops reading when the queue passes the line, and a
// lead close to the line makes the device push back on the socket in the middle
// of a picture packet. A lead of 400 ms is ten chunks, ten below the line.
//
// The count came down from thirty because the chunks doubled: a chunk is 40 ms
// now rather than 20, so eighteen of them hold more audio than thirty of the old
// ones did, for 22.6 KB against 18.9. That is affordable; forty-eight chunks of
// the larger size was not, and the way it failed is worth recording because it
// did not look like memory at all -- every session ended at "RX connect failed
// errno=119" with heap=29268 and min_heap=3932, which reads as a network fault
// and is a heap that ran out before the socket could be opened.
#define PCM_QUEUE 24
// Sessions without a picture before the channel is treated as dead and skipped.
// A channel the viewer chose gets more attempts: stepping away from a deliberate
// choice reads as the button having done nothing.
#define DEAD_CHANNEL_TRIES 3u
#define DEAD_CHANNEL_TRIES_USER 5u
// The proven-channel set stores ids in fixed fields, so a smaller field here
// would silently refuse every id the player is willing to use, and the channel
// would look unproven for ever -- which is the fault this file exists to fix.
_Static_assert(AV_CHANNEL_POLICY_ID_MAX == AV_CHANNEL_ID_MAX,
               "proven-channel id field must match the channel id bound");
// How long a channel change waits for the three workers to retire before giving
// up on them. Every worker sleeps in 5 ms steps and checks STOP, so an ordinary
// exit is one or two ticks; the video worker can be inside an 89 ms JPEG decode
// and the audio worker inside a DMA write, which is what this covers. It is a
// ceiling on a wedged worker, not a schedule.
#define SESSION_DRAIN_MS 1000u
// A second wait, used only when the first expires. SESSION_DRAIN_MS covers every
// ordinary exit with room to spare (measured at 10-37 ms); this covers an
// unlucky overlap with a codec open or a DMA write. See session_drain().
#define SESSION_DRAIN_GRACE_MS 4000u
#define STRIPE_BYTES (AV_WIDTH * AV_STRIPE_ROWS * 2)
// Output level used for the colour-bar screens, where the codec is open with no
// audio fed to it and the only sound is its own noise floor.
//
// Higher than a typical listening level on purpose: it is the noise floor that
// has to be audible here, not a signal, and that floor sits well below the
// signal. Whether this dial moves it at all depends on where the chip generates
// it -- a level control before the noise source scales it, one after does not --
// which cannot be settled by reading the datasheet. It is a named constant so
// the value can be changed after listening; if raising it does nothing, the
// noise is downstream of the control and the hiss has to be generated instead.
#define SETUP_HISS_VOLUME 100u
// The picture arrives at the panel size, or at a size the device enlarges to it
// by one exact ratio. Which ratio is a pair of macros rather than a fact baked
// into these assertions, so that changing the geometry is one edit in
// av_protocol.h and the build fails here if the sizes and the ratio disagree.
_Static_assert(AV_WIDTH * AV_ENLARGE_DEN == AV_VIDEO_WIDTH * AV_ENLARGE_NUM,
               "The panel's width must be the picture's times the enlargement");
_Static_assert((AV_HEIGHT * AV_ENLARGE_DEN == AV_VIDEO_HEIGHT * AV_ENLARGE_NUM) ||
               (AV_VIDEO_HEIGHT <= AV_HEIGHT && AV_ENLARGE_NUM == 1u && AV_ENLARGE_DEN == 1u && (AV_HEIGHT - AV_VIDEO_HEIGHT) % 2u == 0u),
               "The panel's height must match enlarged picture or be centered letterboxed");
_Static_assert(AV_STRIPE_ROWS * AV_ENLARGE_DEN == AV_VIDEO_STRIPE_ROWS * AV_ENLARGE_NUM,
               "One panel stripe must be one picture stripe times the enlargement");
_Static_assert(AV_VIDEO_HEIGHT % AV_VIDEO_STRIPE_ROWS == 0u,
               "The picture must be a whole number of stripes");
_Static_assert(AV_STRIPES * AV_VIDEO_STRIPE_ROWS == AV_VIDEO_HEIGHT,
               "A frame must be a whole number of picture stripes");
#define AV_VIDEO_Y_OFFSET ((AV_HEIGHT - (AV_VIDEO_HEIGHT * AV_ENLARGE_NUM / AV_ENLARGE_DEN)) / 2u)
// Two, because the panel is fed one stripe ahead of the one being built; see
// push_stripe(). It is a constant rather than a keyword argument so the
// allocation log line can name it.
#define STRIPE_BUFFERS 2u
// One stripe buffer holds the index bytes and then the RGB565 expanded from
// them in place, so it has to be twice the pixels of a stripe.
// The buffer holds the incoming stripe at one end and the enlarged panel stripe
// at the other, so it has to be the larger of the two -- which is the panel's,
// and which is what this was already sized for.
_Static_assert(STRIPE_BYTES == AV_WIDTH * AV_STRIPE_ROWS * 2,
               "A stripe buffer must hold one panel stripe");
_Static_assert(AV_STRIPE_PIXELS * 2 <= STRIPE_BYTES,
               "The incoming stripe must fit before it is enlarged");
#define DMA_ESTIMATE_MS 90 // Six 240-frame descriptors / 16 kHz; NOT DMA measurement.
_Static_assert(AV_WIDTH == BSP_LCD_H && AV_HEIGHT == BSP_LCD_W, "Landscape geometry");
_Static_assert(AV_PALETTE_ENTRIES == 256, "An 8-bit index needs 256 colours");
// The panel's floor, and it is asserted rather than trusted because the
// expression printing it has been wrong twice in the same direction: 3 ms for a
// bus that takes 15.4 (then 30.7, at the old 40 MHz clock). A byte is eight bits
// and the clock is in Hz; leave the eight out and the floor reads a tenth of the
// truth, which is worse than no number at all because every later judgement is
// made against it.
//
// 320*240*2*8 bits at 80 MHz is 15360 us. Checked as a whole millisecond figure,
// since the integer division truncates it to 15.
#define PANEL_FLOOR_MS ((AV_WIDTH * AV_HEIGHT * 2 * 8) * 1000u / BSP_LCD_PCLK_HZ)
_Static_assert(PANEL_FLOOR_MS == 15u,
               "The panel floor changed: every per-frame figure is compared against it");
#define AV_SERVER_ADDR_MAX FT_TV_HOST_MAX
typedef struct { char host[AV_SERVER_ADDR_MAX]; uint16_t port; } av_server_addr_t;
static uint8_t *s_lvgl_stripes;
static av_frame_region_t s_frame_regions[AV_FRAME_REGIONS];
static const char *TAG = "ft_tv";
static atomic_uint_least32_t heap_failures,heap_media_failures,heap_failed_bytes,heap_failed_caps;
static atomic_bool heap_media_phase;
static void (*s_host_allocation_failed)(size_t,uint32_t,const char *);
static void tv_heap_ignore(size_t bytes,uint32_t caps,const char *function) {
    (void)bytes;(void)caps;(void)function;
}
static void tv_heap_failed(size_t bytes,uint32_t caps,const char *function) {
    // Allocation failure hooks must not allocate or print. Keep the existing
    // radio OOM recorder intact; expose only counters from the normal sampler.
    atomic_fetch_add(&heap_failures,1u);
    if(atomic_load(&heap_media_phase))atomic_fetch_add(&heap_media_failures,1u);
    atomic_store(&heap_failed_bytes,(uint32_t)bytes);atomic_store(&heap_failed_caps,caps);
    if(s_host_allocation_failed)s_host_allocation_failed(bytes,caps,function);
}
void ft_tv_stop(void);
// What a session knows about reaching the server.
//
// Three states rather than two. "Not yet known" and "failed" are different
// facts, and the screen has to separate them: a device that started dialling a
// moment ago is not a device that cannot connect, and saying the second while
// the first is true sends the viewer to change a setting that was never wrong.
// The connection attempt itself takes up to three seconds, so that window is
// long enough to read.
typedef enum {
    // The session is dialling; nothing has been concluded.
    LINK_TRYING = 0,
    // The socket came up, so the server answered.
    LINK_UP,
    // The attempt is over and the server was not reached. Everything that can
    // go wrong on the way -- no address, a name that will not resolve, a
    // refused or timed-out connection -- ends here, because what the viewer
    // does about all of them is the same: go to the setup page.
    LINK_DOWN,
} link_state_t;

// One audio packet as it arrived: IMA ADPCM, decoded to PCM by the audio task
// just before it is written. The queue holds the compressed form, 324 bytes a
// chunk against the 1280 of the PCM it stands for.
typedef struct { uint32_t pts; uint8_t block[AV_AUDIO_BYTES]; } audio_t;
// Only complete frames cross from the receiver to the display task.
typedef av_frame_stripe_t video_t;
// A queued button gesture. Carrying the event type (not just the key) is what
// lets a click mean "next channel" while a long press means "volume".
typedef struct { bsp_btn_t key; bsp_btn_ev_t ev; } key_ev_t;
// Channel switching reconnects with a new id, so a switch is just a session
// restart. One writer (the key path) sets a pending flag; the session owner
// applies it, which avoids touching the string while a connection is being made.
// The id length lives in av_protocol.h so the parser and this struct agree.
// The display name is kept as well: the overlay shows it, and the server already
// sends it in CONFIG, so no extra request is needed to name a channel.
typedef struct { char id[AV_CHANNEL_ID_MAX]; char name[UI_MENU_NAME_MAX]; } channel_t;
typedef struct {
    EventGroupHandle_t events;
    QueueHandle_t audio, video, keys;
    uint8_t *stripe[2];
    av_frame_ring_t frame_ring;
    av_rx_timing_t rx_timing[RX_PHASES];
    uint32_t rx_stall_us, rx_stall_parts[RX_PHASES]; // guarded by clock_lock
    uint8_t *frame_heap; // bounded supplement, allocated only after CONFIG parsing
    atomic_uint_least32_t frame_ready, frame_overflow, frame_size_max;
    atomic_uint_least32_t frame_receive_max_us, frame_draw_max_us;
    atomic_uint_least32_t frame_submit_gap_max_us, menu_frames, housekeeping_us;
    // Which of the two the next stripe is built in. They alternate; a stripe
    // is only overwritten once the transfer that read it has been drained.
    unsigned stripe_next;
    // Transfers queued and not yet drained. The panel is fed ahead of the
    // stripes being built, so this is the depth of the overlap rather than a
    // flag: one outstanding transfer means the bus is busy while the CPU
    // builds the next stripe.
    unsigned stripe_live;
    // One zlib decompressor for the whole session. It is about 11 KB, so it
    // lives in the session's heap rather than in this struct, which is static
    // and would pay for it at every boot -- including the boots that never
    // reach a channel.
    tinfl_decompressor *inflate;
    // The colours a 256-index picture is drawn with, sent by the server before
    // the first frame because the palette is chosen per channel. Held in WIRE
    // order (each entry byte-swapped, see av_palette_wire_order), which is what
    // av_expand_indexed_wire reads; nothing else uses this table.
    uint16_t *palette; // hot pixel lookups stay in fast SRAM, not RTC state
    bool palette_ready;
    // Stripes drawn since the current frame began. A frame arrives as several
    // packets and the count is what says the last of them has gone out.
    unsigned drawn_stripes;
    // clock_us is the audio playback origin, set when feeding actually starts.
    int64_t clock_us;
    // submitted_samples counts everything written to the audio hardware, and
    // therefore includes the silence the device generates when the queue runs
    // dry. silence_samples is the part of that which the device made up.
    //
    // The two are different quantities and only one of them is on the wire.
    // The sender's timeline advances by real chunks alone -- sound it dropped
    // occupies no session time -- so a clock built from submitted_samples runs
    // permanently ahead of every timestamp that arrives after a gap. The
    // picture then fails the lateness test for ever: measured, the device drew
    // nothing for a minute while the sound kept playing. What the picture is
    // judged against is the PROGRAM position, which is the difference between
    // the two counters.
    atomic_uint_least32_t submitted_samples, silence_samples;
    atomic_uint_least32_t decoded, decode_max_ms, feed_gap_max_ms;
    atomic_uint_least32_t dropped, last_packet_ms;
    atomic_uint_least32_t audio_high, video_high;
    // Where the time inside a frame actually goes. decode_max_ms says how long
    // the worst packet took but not what it was doing, and the two halves want
    // opposite remedies: inflating is processor time, and the panel submission
    // is wire time that only a smaller or less frequent picture can reduce.
    // Counted rather than sampled, so the interval log can divide one by the
    // other and give a rate in bytes per second.
    atomic_uint_least32_t inflate_us, panel_us, inflate_bytes;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    atomic_uint_least32_t rx_cpu_us, video_cpu_us, audio_cpu_us;
#endif
    // The rest of the stripe's cost, split out so the four parts can be read
    // against each other.
    //
    // `inflate_us` and `panel_wait_us` between them answered "how much is
    // decompression and how much is the wire", and left the largest question
    // unasked: the expansion from index bytes to RGB565, the enlargement, and
    // the overlay all happen inside `panel_us` and none of them was timed.
    // They want different remedies -- the expansion is table lookups, the
    // enlargement is byte moves, the overlay is normally nothing at all -- and
    // from outside all three look like "the frame took 60 ms".
    //
    // These four plus the two above should sum to `panel_us`. That sum is its
    // own instrument check: a total larger than the interval it is nested in
    // means a timer was started twice or read after it was reset.
    //
    // The overlay is counted even though it is free on stripes it does not
    // touch, because "the overlay costs nothing" is an assumption this project
    // has already had to correct once.
    atomic_uint_least32_t expand_us, enlarge_us, overlay_us, submit_us;
    // Counted beside panel_us so the mean is over exactly the frames the
    // sum covers. Without it the sum is a number with no denominator, and
    // a sum that does not match the frame count is a broken instrument
    // rather than a slow or a fast device.
    atomic_uint_least32_t panel_frames;
    // How long the picture task spent waiting for the panel's own DMA to
    // finish. This is the number that says whether the SPI transfer is
    // serial with the decoding or already overlapped: if it is close to
    // 153600 bytes at the bus rate -- about 31 ms a frame -- then the
    // processor is standing still beside a DMA engine and there is a
    // frame rate to be had by overlapping them. If it is near zero the
    // transfer is already happening in the background and the drawing
    // time is genuine computation.
    atomic_uint_least32_t panel_wait_us;
    // What actually came off the socket, counted before any decision to draw or
    // drop it. inflate_bytes only sees what reached the decoder, so on a channel
    // that is dropping most of its frames it reports the pipeline's intake and
    // not the link's: the two differ by everything thrown away, and telling them
    // apart is the difference between "the network is slow" and "we are slow".
    atomic_uint_least32_t rx_video_bytes, rx_video_packets, rx_audio_packets;
    // One-second readings for the screen, written by the session loop: frames a
    // second times ten, and bytes a second off the socket, audio included.
    atomic_uint_least32_t fps_x10, rx_bytes_per_s;
    // Where the receive task's time actually goes, in microseconds, measured
    // rather than reasoned about. The three parts answer three different
    // questions and want three different fixes: `io` is time inside recv(),
    // which is the socket and the network; `wait` is time parked on a full
    // queue, which is the consumer being slow; `overhead` is the rest of the
    // loop, which is this chip being slow. Without the split every one of those
    // looks the same from outside -- the picture does not arrive fast enough.
    atomic_uint_least32_t rx_io_us, rx_wait_us, rx_overhead_us, rx_iterations;
    // Why frames do not finish.
    //
    // `decoded` counts completed pictures and `dropped` counts the ones thrown
    // away, and when both are near zero while packets are plainly arriving,
    // neither number says what happened to them. These four do: a frame that
    // opened, one that was skipped as late, one whose start packet never came,
    // and the highest stripe count any frame reached. The last is the one that
    // distinguishes "the frame arrived in pieces that were counted separately"
    // from "the frame arrived but the count was cleared before it finished".
    atomic_uint_least32_t rx_frame_starts, rx_frame_skipped, rx_stray_packets,
        rx_max_stripes, rx_nobuf_packets;
    // The longest the receive loop went without a packet header, in ms.
    //
    // Every other receive figure here is an average or a total, and both hide
    // the shape that matters. Measured across a ten-second interval: 502
    // packets read, 1.4 s of I/O, so the loop is busy 14% of the time -- and no
    // counter said where the other 8.6 s went. A single longest gap answers it
    // without needing a tracer: if the gap is a few tens of milliseconds the
    // loop is being served and the shortfall is elsewhere; if it is hundreds,
    // the loop is stopped, and everything downstream -- audio underruns, the
    // sender's socket filling, its writes taking 400 ms -- follows from that
    // one number.
    atomic_uint_least32_t rx_header_gap_max_ms;
    atomic_uchar volume;
    // The server sends the channel list in CONFIG; the device only stores ids
    // and never hardcodes them, so adding a channel needs no reflash.
    channel_t list[AV_CHANNEL_MAX];
    // Which part of the frame currently being received this packet is, read by
    // the video task before the wait that follows. A frame arrives as five
    // consecutive packets sharing one timestamp, and the four that continue it
    // are not yet a frame: there is nothing to draw until the last has landed.
    // The task has to know that before it waits, because what it waits for --
    // this frame, or the next one -- depends on the answer.
    // Draws a frame only to abandon it part way -- the clock has moved past it
    // -- would leave the top of the picture showing this frame and the bottom
    // showing the last one. Once a frame is dropped, the rest of its packets
    // are discarded unread until the next one opens; the flag says so.
    bool skipping;
    // Unsigned, not uchar: AV_CHANNEL_MAX is above 255, and an 8-bit count
    // silently wrapped, which would have made a 300-channel list look like 44.
    atomic_uint count, index;
    atomic_bool switch_pending;
    // Set once the first PCM/JPEG arrives, which switches the idle watchdog
    // from the startup allowance to the normal inter-packet gap.
    atomic_bool media_started;
    // s.channel is what hello() sends; s.pending is what a key press asks for.
    // s.channel has two writers, never at the same time: the owner task copies
    // pending -> channel between sessions, and the receive task overwrites it
    // with the channel the server confirms while that session is running. Both
    // sit on opposite sides of a session boundary, and the owner only reads the
    // string while building a connection, so no lock is needed.
    char channel[AV_CHANNEL_ID_MAX], pending[AV_CHANNEL_ID_MAX];
    // The overlay. Only the session owner and the video task touch it, and they
    // are the same thread of control: the owner changes the view between frames,
    // the video task reads it while drawing. No separate lock is needed because
    // the video task never blocks on the owner for longer than one frame.
    ui_menu_t menu;
    bool quality_pending;  // owner applies/saves only after the old workers retire
    // A CONFIG arrives on the receive task but the menu is read by the video
    // task while it draws. The two must not touch the same strings at once, so
    // the receive task only raises a flag; the session owner builds the menu
    // from s.list between frames, where nothing is drawing from it.
    atomic_bool list_dirty;
    atomic_bool quality_available;
    // Battery percentage, refreshed on its own slow schedule: the fuel gauge is
    // read over I2C on the shared bus, which is far too slow for the frame path.
    atomic_int battery_soc;
    // Consecutive sessions that ended without ever showing a picture. A source
    // that has gone dead fails instantly, so without this the device reconnects
    // to the same dead channel forever and the screen never leaves the test
    // pattern. Counting lets it step past a channel that does not work.
    //
    // What it must not do is step past a channel that works, and the count alone
    // cannot tell the two apart: a session interrupted by a source stutter also
    // ends with no picture. `proven` is what separates them. Only the session
    // owner touches this, between sessions, so it needs no lock.
    unsigned dead_streak;
    av_channel_policy_t proven;
    // What this session knows about reaching the server.
    //
    // Written by the receive task as it dials, and read by the video task while
    // it draws and by the session owner between sessions.
    atomic_int link;
    // Set when the viewer picked the channel themselves. A channel they asked for
    // gets more attempts before being skipped, because jumping away from a
    // deliberate choice looks like the button did nothing.
    atomic_bool chosen_by_user;
    // Raised when the volume or the backlight changed. The key path only sets
    // it: writing to NVS on the button task would delay the next press, so the
    // session loop does the write when it next comes round.
    atomic_bool settings_dirty;
} player_t;
// Heap-held for the life of one TV visit: allocated by ft_tv_start, freed when
// the TV task finishes, so FoloTalk does not carry 6 KB of TV state on Home.
static player_t *s_player;
// Guards s_player against the button task, which posts keys concurrently.
static portMUX_TYPE player_lock = portMUX_INITIALIZER_UNLOCKED;
#define s (*s_player)
// The channel whose waiting screen is currently on the panel. A reconnect to the
// same channel leaves the picture alone instead of blanking it.
static char shown_channel[AV_CHANNEL_ID_MAX];
// Whether the panel has ever been painted. Everything before the first frame
// would otherwise show uninitialised display memory, so the first session must
// draw the waiting screen even though its channel id is still empty and equals
// shown_channel. See the use site.
static bool waiting_drawn;
// Whether the black bars above and below the picture currently carry readings,
// and when they were last painted. The picture never covers the bars, so during
// playback they are only touched by refresh_info_bars().
static bool bars_drawn;
static int64_t bars_painted_us;
// Which overlay the waiting screen was last painted with, and when.
//
// The waiting screen is a still image, so it is painted when something it shows
// changes rather than on every pass: a full repaint pushes 150 KB over the
// panel's bus, which is not something to do dozens of times a second for a
// picture that has not moved. The overlay is what changes -- the status page's
// readings, the highlight in the list -- which is why it is repainted while one
// is open.
static int waiting_view=-1;
static int waiting_link=-1;
// The address the last connection attempt used, as "host:port".
//
// Kept so that dialling again is not mistaken for news. Every session begins by
// connecting, so a device that cannot reach its server would otherwise announce
// "connecting" once per attempt -- at about a second apart, that is a message
// flashing on and off for ever, and it tells the viewer nothing they were not
// already told by the failure underneath it. Dialling is worth showing only when
// there is something new to say it about: the first attempt, or a different
// address.
static char last_attempt_addr[AV_SERVER_ADDR_MAX+8];
static int64_t waiting_painted_us;
// How often the waiting screen is refreshed while an overlay is up. Fast enough
// that a key press looks immediate, slow enough that the bus is left alone
// between presses.
#define WAITING_REFRESH_MS 200
// The setup page takes over the whole device, so every key means something
// different while it is up. It is set before the panel is claimed and cleared
// before playback, and it is read by the key handler on the button task.
static atomic_bool s_setup_active; // unused in FoloTalk; kept so key handling stays identical
// The viewer asked to leave setup. Read by the setup loop, which owns the
// decision to go back to playback.
static atomic_bool s_setup_leave;
// The viewer asked to open the setup screen, set from the status page and
// consumed at a session boundary -- the same shape as switch_pending, and for
// the same reason: the request arrives while a session is running, and the
// session has to be over before its memory can be given to the access point.
static atomic_bool s_setup_request;
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
// Guards the channel id table, which two tasks touch: the receive task rebuilds
// it from CONFIG while the owner task snapshots it on a key press. Publishing
// count=0 first is not enough on its own, because a reader can pass the old
// count check just before the writer starts overwriting the ids.
static portMUX_TYPE list_lock = portMUX_INITIALIZER_UNLOCKED;
static bool stopping(void) { return xEventGroupGetBits(s.events) & STOP; }
static bool rx_done(void) { return xEventGroupGetBits(s.events) & RX_DONE; }
static void fail(const char *reason) {
    ESP_LOGW(TAG, "Session reset: %s", reason); // Never log remote JSON/secrets.
    xEventGroupSetBits(s.events, STOP);
}
// Two complete descriptors; compressed bytes live in borrowed memory regions.
#define AV_VIDEO_ITEMS 2u
#define AV_STRIPE_CMAX 4096u
#define AV_VIDEO_WAIT_US 100000
_Static_assert(AV_CONTROL_MAX < FT_TV_WORKSPACE_BYTES, "CONFIG terminator must fit");
static void ring_release(const video_t *v) {
    av_frame_ring_release(&s.frame_ring,v->release_end);
}
static bool video_enqueue(const av_complete_frame_t *frame) {
    if(!av_frame_complete(frame)) return false;
    int64_t began=esp_timer_get_time(),give_up=began+AV_VIDEO_WAIT_US;
    bool sent=false;
    while(!stopping()) {
        if(xQueueSend(s.video,frame,0)) { sent=true;break; }
        if(esp_timer_get_time()>=give_up) break;
        vTaskDelay(1);
    }
    av_rx_timing_add(&s.rx_timing[RX_FRAME_QUEUE],(uint32_t)(esp_timer_get_time()-began));
    return sent;
}
static void frame_max(atomic_uint_least32_t *value,uint32_t sample) {
    uint_least32_t old=atomic_load(value);
    while(old<sample && !atomic_compare_exchange_weak(value,&old,sample)) {}
}
static bool delay_until(int64_t us) {
    while (!stopping() && esp_timer_get_time()<us) vTaskDelay(pdMS_TO_TICKS(5));
    return !stopping();
}
static int64_t estimated_pts(void) {
    taskENTER_CRITICAL(&clock_lock);
    int64_t origin=s.clock_us;
    uint32_t samples=s.submitted_samples;
    uint32_t silence=s.silence_samples;
    taskEXIT_CRITICAL(&clock_lock);
    int64_t wall=(esp_timer_get_time()-origin)/1000;
    // Program position, not playback position. Silence this device generated
    // was never on the wire, so it must not push this clock past the
    // timestamps that arrive on it: doing so is what left every frame after a
    // gap more than 100 ms "late" and therefore dropped, while the sound
    // carried on. The two counters are reported separately so the difference
    // is visible rather than inferred.
    uint32_t program=samples>silence?samples-silence:0u;
    int64_t submitted=(int64_t)program*1000/16000;
    return wall<submitted ? wall : submitted;
}
static void on_key(bsp_btn_t key, bsp_btn_ev_t ev, void *ctx) {
    (void)ctx;
    // Every gesture this firmware uses must be forwarded: a click steps a
    // channel, a long press is volume or the menu, a double click is the status
    // page. Callbacks run in the button task, so only enqueue and drop when
    // full; never block or do work here.
    if (ev!=BSP_BTN_CLICK && ev!=BSP_BTN_LONG && ev!=BSP_BTN_DOUBLE) return;
    key_ev_t item={.key=key,.ev=ev};
    taskENTER_CRITICAL(&player_lock);
    QueueHandle_t keys=s_player ? s_player->keys : NULL;
    if (keys) xQueueSendFromISR(keys,&item,NULL); // non-blocking; legal inside the lock
    taskEXIT_CRITICAL(&player_lock);
}
// Wi-Fi belongs to FoloTalk: the link is up before TV starts, and a lost
// network surfaces here as a failed connect or a stalled read.
// Receiver-owned diagnostics: metadata only, never addresses/secrets/payload.
static const char *s_rx_stage;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
// Per-task accumulated scheduled time (ESP timer, microseconds). Unlike the
// wall timers around inflate/recv, this excludes time spent in other tasks.
// Snapshot only our own TCB; never walk all tasks or scan their stacks.
static uint32_t own_cpu_us(void) {
    TaskStatus_t status;
    vTaskGetInfo(NULL,&status,pdFALSE,eRunning);
    return (uint32_t)status.ulRunTimeCounter;
}
static uint32_t named_cpu_us(const char *name) {
    TaskHandle_t task=xTaskGetHandle(name);
    if(!task)return 0;
    TaskStatus_t status;
    vTaskGetInfo(task,&status,pdFALSE,eInvalid);
    return (uint32_t)status.ulRunTimeCounter;
}
#endif



// Socket owner only. An absolute deadline also covers all discard fragments.
// A single syscall's cost, in core cycles. Declared here rather than beside the
// benchmark block further down because io_until() needs it and that is far
// earlier in the file; the guard matches the only place that calls it.
//
// esp_cpu_get_cycle_count() advances every clock whether the core is working or
// not, so it must wrap a specific operation and never a blocking wait -- around
// a recv() it measures the copy lwIP did, around the loop that contains one it
// would measure wall time instead.
static bool io_until(int fd, void *buf, size_t n, bool sending, int64_t deadline) {
    size_t off=0;
    int64_t started=esp_timer_get_time();
    const char *reason="deadline";
    int error=0;
    while (off<n && !stopping() && esp_timer_get_time()<deadline) {
        int64_t call_start=esp_timer_get_time();
        ssize_t r=sending ? send(fd,(uint8_t *)buf+off,n-off,0) : recv(fd,(uint8_t *)buf+off,n-off,0);
        int io_errno=errno;
        if(!sending)av_rx_timing_add(&s.rx_timing[RX_SOCKET_CALL],(uint32_t)(esp_timer_get_time()-call_start));
        errno=io_errno;
        if (r>0) off+=(size_t)r;
        else if (r==0) { reason="EOF"; break; }
        else if (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) {
            error=errno; reason="errno"; break;
        } else {
            // Wait for the socket to become ready instead of sleeping a fixed
            // five milliseconds and trying again.
            //
            // That sleep was the reason the picture would not run faster than
            // about one frame a second, and it hid well: an empty read is the
            // normal case here, because TCP hands over one segment at a time and
            // a packet spans several. Every one of those gaps cost 5 ms, so the
            // receive task could not exceed roughly a hundred packets a second
            // however fast the link was -- measured, 14 packets a second, with
            // the server queueing frames as fast as ffmpeg produced them and the
            // channel tearing down every eight seconds for want of audio.
            //
            // It went unnoticed while a frame was a single packet at 12 frames a
            // second, where 5 ms between packets costs nothing. At fifteen
            // packets a frame it is most of the budget.
            //
            // Capped at 100 ms so the loop still notices `stopping()` promptly
            // even when nothing ever arrives.
            int64_t left_us=deadline-esp_timer_get_time();
            struct timeval tv;
            if (left_us>100000) left_us=100000;
            tv.tv_sec=(time_t)(left_us/1000000);
            tv.tv_usec=(suseconds_t)(left_us%1000000);
            fd_set readfds, writefds;
            FD_ZERO(&readfds); FD_ZERO(&writefds);
            if (sending) FD_SET(fd,&writefds); else FD_SET(fd,&readfds);
            int64_t select_start=esp_timer_get_time();
            select(fd+1,sending?NULL:&readfds,sending?&writefds:NULL,NULL,&tv);
            if(!sending)av_rx_timing_add(&s.rx_timing[RX_SOCKET_SELECT],(uint32_t)(esp_timer_get_time()-select_start));
        }
    }
    if(off==n) return true;
    if(stopping()) reason="cancelled";
    ESP_LOGW(TAG,"RX_IO stage=%s reason=%s errno=%d bytes=%u/%u elapsed_ms=%"PRId64,
        s_rx_stage,reason,error,(unsigned)off,(unsigned)n,(esp_timer_get_time()-started)/1000);
    return false;
}
static bool io_all(int fd, void *buf, size_t n, bool sending, unsigned deadline_ms) {
    return io_until(fd,buf,n,sending,esp_timer_get_time()+(int64_t)deadline_ms*1000);
}
// Only the owner writes credentials, between sessions with no media workers.
static av_server_addr_t s_target;
static char s_ticket[FT_TV_TICKET_MAX];
static ft_tv_connection_cb_t s_connection_provider;
static void *s_connection_provider_ctx;
static bool s_refreshing; // owner task only; bounds the cancellation query's lifetime
static uint8_t *s_control;
static uint8_t s_brightness=50;
static unsigned s_quality=AV_QUALITY_DEFAULT;

static bool connection_valid(const ft_tv_connection_t *c) {
    const char *host_end=memchr(c->host,0,sizeof(c->host));
    const char *ticket_end=memchr(c->ticket,0,sizeof(c->ticket));
    if(!host_end || host_end==c->host || !ticket_end || ticket_end==c->ticket || !c->port)
        return false;
    size_t label=0;
    for(const char *p=c->host;p<host_end;p++) {
        char ch=*p;
        if(ch=='.') {
            if(!label || p[-1]=='-')return false;
            label=0;
        } else {
            if(!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') ||
                 (ch>='0' && ch<='9') || (ch=='-' && label)))return false;
            label++;
        }
    }
    if(!label || host_end[-1]=='-')return false;
    for(const char *p=c->ticket;p<ticket_end;p++)
        if((unsigned char)*p<33 || (unsigned char)*p>126)return false;
    return true;
}

static esp_err_t refresh_connection(void) {
    if(!s_connection_provider)return ESP_OK;
    // No session/media allocations exist while the provider needs TLS memory.
    ft_tv_connection_t next={0};
    s_refreshing=true;
    int64_t elapsed=0;
    esp_err_t err=ESP_ERR_INVALID_STATE;
    if(!ft_tv_connection_cancelled()) {
        int64_t began=esp_timer_get_time();
        err=s_connection_provider(s_connection_provider_ctx,&next,FT_TV_CONNECTION_TIMEOUT_MS);
        elapsed=esp_timer_get_time()-began;
    }
    bool cancelled=ft_tv_connection_cancelled();
    s_refreshing=false;
    if(cancelled)err=ESP_ERR_INVALID_STATE;
    else if(elapsed>(int64_t)FT_TV_CONNECTION_TIMEOUT_MS*1000)err=ESP_ERR_TIMEOUT;
    else if(err==ESP_OK && !connection_valid(&next))err=ESP_ERR_INVALID_ARG;
    if(err==ESP_OK) {
        // Validate the entire tuple before publishing any field to the next RX.
        memcpy(s_target.host,next.host,sizeof(s_target.host));
        s_target.port=next.port;
        memcpy(s_ticket,next.ticket,sizeof(s_ticket));
    }
    memset(&next,0,sizeof(next));
    return err;
}

static void quality_load(void) {
    nvs_handle_t handle;
    uint8_t value=AV_QUALITY_DEFAULT;
    if (nvs_open("ft_tv",NVS_READONLY,&handle)==ESP_OK) {
        (void)nvs_get_u8(handle,"quality",&value);
        nvs_close(handle);
    }
    s_quality=value<AV_QUALITY_COUNT?value:AV_QUALITY_DEFAULT;
}
static void quality_save(void) {
    nvs_handle_t handle;
    esp_err_t err=nvs_open("ft_tv",NVS_READWRITE,&handle);
    if (err==ESP_OK) {
        err=nvs_set_u8(handle,"quality",(uint8_t)s_quality);
        if(err==ESP_OK) err=nvs_commit(handle);
        nvs_close(handle);
    }
    if(err!=ESP_OK) ESP_LOGW(TAG,"Quality preference save failed: %s",esp_err_to_name(err));
}
static bool server_address(av_server_addr_t *out) {
    if (!s_target.host[0]) return false;
    *out=s_target;
    return true;
}


static int connect_server(void) {
    // A measurement build may ask for the cable; every other build is the
    // product and the product is a network set-top box.
    //
    // This was written the other way round first -- USB whenever a host was
    // attached, WiFi otherwise -- and that is a different device, not a faster
    // one. It would have changed what the box does the moment somebody plugged
    // it into a computer to charge it, which is a behaviour change nobody asked
    // for and the opposite of keeping the network path intact. The cable is a
    // second transport to measure against, so it is behind a switch that is off
    // unless a build turns it on. See CONFIG_AV_USB_TRANSPORT.
    av_server_addr_t target;
    if (!server_address(&target)) {
        ESP_LOGW(TAG,"No server address; enter one on the setup page");
        return -1;
    }
    // Resolved rather than converted: an address typed into the page may be a
    // name, and a name is what survives the server moving to a different lease.
    // A literal costs nothing extra -- lwIP recognises one and skips the lookup.
    struct addrinfo hints={0}, *found=NULL;
    hints.ai_family=AF_INET;
    hints.ai_socktype=SOCK_STREAM;
    char service[6];
    snprintf(service,sizeof(service),"%u",(unsigned)target.port);
    int gai=getaddrinfo(target.host,service,&hints,&found);
    if (gai!=0 || !found) {
        ESP_LOGW(TAG,"Cannot resolve %s: %d",target.host,gai);
        if (found) freeaddrinfo(found);
        return -1;
    }
    int fd=socket(found->ai_family,found->ai_socktype,found->ai_protocol);
    if (fd<0) { freeaddrinfo(found); return -1; }
    // The address is used only to dial, so the list is released before the wait:
    // holding it across the three seconds below would keep DNS memory occupied
    // for no reason.
    struct sockaddr_storage peer;
    memcpy(&peer,found->ai_addr,found->ai_addrlen);
    socklen_t peer_len=found->ai_addrlen;
    freeaddrinfo(found);
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) { close(fd); return -1; }
    if (connect(fd,(struct sockaddr *)&peer,peer_len)<0 && errno!=EINPROGRESS) {
        close(fd); return -1;
    }
    int64_t end=esp_timer_get_time()+3000000;
    while (!stopping() && esp_timer_get_time()<end) {
        fd_set wr; FD_ZERO(&wr); FD_SET(fd,&wr);
        struct timeval tv={.tv_usec=50000};
        int r=select(fd+1,NULL,&wr,NULL,&tv);
        if (r<0 && errno!=EINTR) break;
        if (r>0) {
            int error=0; socklen_t len=sizeof(error);
            if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)==0 && !error) return fd;
            break;
        }
    }
    close(fd); return -1;
}
static bool json_number(const cJSON *j, const char *key, uint32_t expected) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsNumber(v) && v->valuedouble==(double)expected;
}
// A figure that must be sane but is the server's to choose, rather than one
// both ends have to agree on to the unit.
static bool json_between(const cJSON *j, const char *key, uint32_t low, uint32_t high) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsNumber(v) && v->valuedouble>=(double)low && v->valuedouble<=(double)high;
}
static bool json_string(const cJSON *j, const char *key, const char *expected) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) && v->valuestring && !strcmp(v->valuestring,expected);
}
// The product refuses an announced rate above 30. A measurement build that
// carries the media over USB lifts that, so the cable can be pushed past what the
// panel was sized for and the point where it stops keeping up can be found.
#define AV_CONFIG_FPS_MAX 30
static bool config_valid(char *buf, size_t n, uint32_t session) {
    if (!av_json_depth_safe(buf,n,4)) {
        ESP_LOGW(TAG,"CONFIG rejected: json depth/parse guard");
        return false;
    }
    buf[n]=0;
    const char *end=NULL;
    cJSON *j=cJSON_ParseWithLengthOpts(buf,n+1,&end,true);
    bool ok=cJSON_IsObject(j) && json_number(j,"width",AV_VIDEO_WIDTH) && json_number(j,"height",AV_VIDEO_HEIGHT) &&
        // The frame rate is checked for sanity, not for equality, and that is a
        // deliberate change from how every other field here is treated.
        //
        // The device does not pace by this number -- every frame carries its own
        // timestamp and is scheduled against the audio clock -- so the two ends
        // have no need to agree on it to the unit. Requiring an exact match made
        // the rate a property of the firmware: trying 6 fps instead of 4 meant
        // rebuilding and reflashing the device, and the measurement being asked
        // for was always "how fast can this link actually go", which is exactly
        // the question a rebuild destroys the chance of asking cheaply.
        //
        // The bounds still catch a server sending nonsense, and the ceiling
        // keeps a stream from announcing a rate the panel could never draw.
        json_between(j,"fps",1,AV_CONFIG_FPS_MAX) && json_number(j,"sample_rate",16000) && json_number(j,"channels",1) &&
        // sample_bits is the decoded sound, not what travels: the packets are
        // ADPCM, and "audio_codec" is what says so. A server that still sends
        // PCM would otherwise connect, pass every other check, and be dropped
        // at its first audio packet for a length no header can have -- with the
        // log saying nothing about why.
        json_number(j,"sample_bits",16) && json_string(j,"audio_codec","ima_adpcm") &&
        json_number(j,"audio_chunk_ms",AV_AUDIO_MS) &&
        json_number(j,"video_max_bytes",AV_VIDEO_MAX) && json_number(j,"session",session) &&
        // The picture's stripe height, not the panel's. The two were the same
        // number until the picture started arriving smaller than the panel, and
        // the field means what the server cut the frame into -- so it has to be
        // checked against what the picture supplies, or a server and a firmware
        // that agree on everything visible still refuse to talk. Measured: the
        // mismatch cost every session at once, connecting and dropping within a
        // second with no media sent, which reads as a broken link rather than as
        // a rejected configuration.
        json_number(j,"stripe_rows",AV_VIDEO_STRIPE_ROWS);
    const cJSON *quality=cJSON_GetObjectItemCaseSensitive(j,"quality");
    if(quality && !json_string(j,"quality",av_quality_id(s_quality))) {
        ESP_LOGW(TAG,"CONFIG rejected: quality");
        ok=false;
    }
    if(ok) atomic_store(&s.quality_available,quality!=NULL);
    const cJSON *delay=cJSON_GetObjectItemCaseSensitive(j,"start_delay_ms");
    if (delay && !json_number(j,"start_delay_ms",200)) {
        ESP_LOGW(TAG,"CONFIG rejected: start_delay_ms");
        ok=false;
    }
    // Named on rejection, so the log says which field disagreed rather than
    // only that one did.
    //
    // This exists because a rejected CONFIG does not look like a rejected
    // CONFIG. The device connects, authenticates, and drops within a second
    // having sent nothing, which from outside is a broken link, an unstable
    // network, or a firmware fault -- and this project has chased that shape
    // three times. The last one was not a field at all: cJSON could not
    // allocate, returned NULL, and every lookup against it came back empty, so
    // the log said every field was missing and the real cause was 16 KB of
    // video buffers that had been added to a heap the channel list needed.
    if (!ok) {
        if (!j) {
            ESP_LOGW(TAG,"CONFIG rejected: parse failed, heap=%u largest=%u",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        } else {
            const uint32_t want[]={AV_VIDEO_WIDTH,AV_VIDEO_HEIGHT,AV_VIDEO_STRIPE_ROWS,
                                   AV_VIDEO_MAX,AV_AUDIO_MS};
            const char *names[]={"width","height","stripe_rows","video_max_bytes",
                                 "audio_chunk_ms"};
            for (unsigned i=0;i<5;i++) {
                const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,names[i]);
                if (!cJSON_IsNumber(v) || v->valuedouble!=(double)want[i])
                    ESP_LOGW(TAG,"CONFIG field %s: server=%d device=%u",names[i],
                             cJSON_IsNumber(v)?(int)v->valuedouble:-1,(unsigned)want[i]);
            }
            if (!json_string(j,"audio_codec","ima_adpcm")) {
                const cJSON *c=cJSON_GetObjectItemCaseSensitive(j,"audio_codec");
                ESP_LOGW(TAG,"CONFIG field audio_codec: server=%s device=ima_adpcm",
                         cJSON_IsString(c)&&c->valuestring?c->valuestring:"(missing)");
            }
            const cJSON *ss=cJSON_GetObjectItemCaseSensitive(j,"session");
            if (!cJSON_IsNumber(ss) || ss->valuedouble!=(double)session)
                ESP_LOGW(TAG,"CONFIG session: server=%d header=%u",
                         cJSON_IsNumber(ss)?(int)ss->valuedouble:-1,(unsigned)session);
        }
    }
    if (ok) {
        // "channel_list" is distinct from "channels", which is the audio channel
        // count checked above. Reusing one key for both made every CONFIG fail
        // validation, so keep them separate.
        const cJSON *list=cJSON_GetObjectItemCaseSensitive(j,"channel_list");
        if (cJSON_IsArray(list)) {
            // Adopt the channel the server says it is actually serving. The
            // device asks for a name it may not know (empty on the first
            // connection, or an id the server no longer carries) and the server
            // answers with what it chose. Without this the device's idea of "the
            // channel I am on" stayed empty, so its index lookup failed and the
            // very first press jumped to the top of the list rather than the
            // next entry. Written before the table swap so the index computed
            // below already uses the confirmed name.
            const cJSON *current=cJSON_GetObjectItemCaseSensitive(j,"channel");
            if (cJSON_IsString(current) && current->valuestring) {
                size_t len=strlen(current->valuestring);
                if (len>0 && len<AV_CHANNEL_ID_MAX) {
                    bool printable=true;
                    for (size_t i=0;i<len;i++) {
                        char c=current->valuestring[i];
                        if (c<'!' || c>'~') { printable=false; break; }
                    }
                    if (printable) memcpy(s.channel,current->valuestring,len+1);
                }
            }
            // Parsed straight into the published table, with the count set to
            // zero for the duration of the walk.
            //
            // There used to be a private table of AV_CHANNEL_MAX entries --
            // 8192 bytes -- allocated here, filled from the tree, and copied in
            // at the end under the lock, so that a reader kept the old list
            // until the new one was whole. That allocation was the one that
            // failed, and it failed on the channel list the product actually
            // ships: "No memory to parse the channel list: wanted 8192 B,
            // heap=13192 largest=7680" -- 512 bytes short of contiguous room,
            // because the cJSON tree for 127 channels is itself live at that
            // moment and sits between the free blocks. The session then never
            // started at all, and the screen sat on the waiting picture.
            //
            // Publishing the zero first costs the readers the list while the
            // walk runs, and that is a smaller thing than it sounds: the walk
            // is over a tree already in memory, it happens once per session,
            // and a reader that sees count==0 reads nothing rather than
            // something torn. The old list was not really being kept whole
            // either -- it was kept until the new one was ready, which on a
            // channel change means showing the list you just left.
            //
            // The table is in the session struct and not on the stack: at 64
            // bytes an entry it is far larger than this task's 5 KB stack, and
            // putting it here crashed the device on every CONFIG the moment the
            // channel limit was raised above a handful.
            taskENTER_CRITICAL(&list_lock);
            atomic_store(&s.count,0);
            taskEXIT_CRITICAL(&list_lock);
            memset(s.list,0,sizeof(s.list));
            unsigned n=0;
            const cJSON *entry=NULL;
            cJSON_ArrayForEach(entry,list) {
                const cJSON *id=entry ? cJSON_GetObjectItemCaseSensitive(entry,"id") : NULL;
                if (!cJSON_IsString(id) || !id->valuestring) continue;
                size_t len=strlen(id->valuestring);
                // Bounded like every other control input: an oversized or
                // non-printable id is skipped, never truncated into the table.
                if (n>=AV_CHANNEL_MAX || len==0 || len>=AV_CHANNEL_ID_MAX) continue;
                bool printable=true;
                for (size_t i=0;i<len;i++) {
                    char c=id->valuestring[i];
                    if (c<'!' || c>'~') { printable=false; break; }
                }
                if (!printable) continue;
                memcpy(s.list[n].id,id->valuestring,len+1);
                // The display name is optional: an older server may not send it,
                // and the overlay falls back to the id when it is absent. It is
                // free-form UTF-8, so only the length is bounded here.
                const cJSON *name=entry ? cJSON_GetObjectItemCaseSensitive(entry,"name") : NULL;
                if (cJSON_IsString(name) && name->valuestring) {
                    strncpy(s.list[n].name,name->valuestring,sizeof(s.list[n].name)-1);
                }
                n++;
            }
            if (n) {
                unsigned index=0;
                // Pointers only, a quarter the size of the table they point
                // into. The index is a convenience, not correctness: if this
                // last small allocation fails the list is still published and
                // the device starts on its first entry rather than refusing
                // the session over it.
                const char **ids=heap_caps_malloc(sizeof(char *)*n,MALLOC_CAP_8BIT);
                if (ids) {
                    for (unsigned i=0;i<n;i++) ids[i]=s.list[i].id;
                    index=av_channel_index_of(ids,n,s.channel);
                    free(ids);
                } else {
                    ESP_LOGW(TAG,"No memory to index the channel list; starting at the top");
                }
                if (index>=n) index=0; // current channel was removed from the list
                // Publish index before count: a reader that sees the new count
                // must already see the index that belongs with it, otherwise it
                // can step from a stale position.
                taskENTER_CRITICAL(&list_lock);
                atomic_store(&s.index,(unsigned char)index);
                atomic_store(&s.count,(unsigned int)n);
                taskEXIT_CRITICAL(&list_lock);

                // The overlay is installed by the session owner, not here: the
                // menu is read by the video task while it draws, and a partial
                // copy would be drawn from.
                atomic_store(&s.list_dirty,true);
            } else {
                ESP_LOGW(TAG,"CONFIG carried no usable channel entries");
            }
        }
    }
    if (ok) {
        const cJSON *fps=cJSON_GetObjectItemCaseSensitive(j,"fps");
        ESP_LOGI(TAG,"CONFIG_ACCEPT quality=%s fps=%d",quality?av_quality_id(s_quality):"legacy",fps->valueint);
    }
    cJSON_Delete(j);
    return ok;
}
static bool hello(int fd) {
    cJSON *j=cJSON_CreateObject();
    if (!j) return false;
    // The requested channel travels in the handshake, so switching channels is
    // simply a new connection. The server falls back to its default if the id
    // is unknown, which keeps an older device working against a newer server.
    //
    // The token is sent only when this build has one. Omitting it and sending an
    // empty string look the same on the wire only if the server treats them
    // alike, and it does not: the field's presence is what says "here is my
    // token", so an empty one would be a claim rather than an absence.
    bool ok=cJSON_AddNumberToObject(j,"version",1) &&
        cJSON_AddStringToObject(j,"channel",s.channel) &&
        cJSON_AddStringToObject(j,"quality",av_quality_id(s_quality)) &&
        cJSON_AddStringToObject(j,"token",s_ticket)!=NULL;
    char *body=ok ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!body) return false;
    size_t n=strlen(body);
    uint8_t wire[AV_HEADER_BYTES];
    av_header_t h={.type=AV_HELLO,.length=n}; av_header_encode(wire,&h);
    ok=n<=AV_CONTROL_MAX && io_all(fd,wire,sizeof(wire),true,1000) && io_all(fd,body,n,true,1000);
    // Best effort: do not leave token JSON in a reusable heap block.
    memset(body,0,n); cJSON_free(body);
    return ok;
}
// After CONFIG, the control workspace contains queued compressed stripes.
// Never read an ERROR body into it: the video task may still own those bytes.
// av_stream_accept rejects a second CONFIG before reaching this function.
static void ensure_frame_capacity(void) {
    // UI history changes the borrowed LVGL block. The fixed-source baseline
    // borrowed only 29,096 B while complete frames needed over 30 KB. Waiting
    // cannot make such a frame fit. Supplement only the deficit to 32 KiB,
    // after the transient CONFIG JSON tree has been freed, never before it.
    const uint32_t target=32768u, max_extra=8192u, heap_reserve=12288u;
    if(s.frame_ring.capacity>=target || s.frame_heap || s.frame_ring.head ||
       atomic_load(&s.frame_ring.tail))return;
    uint32_t need=target-s.frame_ring.capacity;
    if(need>max_extra || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<need+heap_reserve) {
        ESP_LOGW(TAG,"Frame supplement unavailable: capacity=%u need=%u",(unsigned)s.frame_ring.capacity,(unsigned)need);
        return;
    }
    s.frame_heap=heap_caps_malloc(need,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!s.frame_heap) {
        ESP_LOGW(TAG,"Frame supplement allocation failed: need=%u",(unsigned)need);
        return;
    }
    // No video has been published yet; only the receiver accesses the ring.
    s_frame_regions[AV_FRAME_REGIONS-1]=(av_frame_region_t){s.frame_heap,need};
    bool ok=av_frame_ring_init(&s.frame_ring,s_frame_regions);
    if(!ok) {
        free(s.frame_heap);s.frame_heap=NULL;
        s_frame_regions[AV_FRAME_REGIONS-1]=(av_frame_region_t){0};
        (void)av_frame_ring_init(&s.frame_ring,s_frame_regions);
    }
    ESP_LOGI(TAG,"Frame capacity after CONFIG=%u supplemental_heap=%u",(unsigned)s.frame_ring.capacity,ok?(unsigned)need:0u);
}
static bool receive_config(int fd, char *control, const av_header_t *h) {
    s_rx_stage=h->type==AV_ERROR ? "server-error" : "control-read";
    if (h->type!=AV_CONFIG) return false;
    if (!io_all(fd,control,h->length,false,AV_READ_DEADLINE_MS)) return false;
    s_rx_stage="config-validation";
    if(!config_valid(control,h->length,h->session))return false;
    atomic_store(&heap_media_phase,true);
    ensure_frame_capacity();
    return true;
}

static void receive_task(void *arg) {
    (void)arg;
    s_rx_stage="connect";
    av_header_t h={0};
    av_stream_t stream={0};
    av_complete_frame_t pending={0};
    bool discard_frame=false;
    int64_t frame_receive_started=0;
    // Borrowed from the disconnected social receiver. CONFIG is copied into
    // session state before media reuses this storage as the compressed ring.
    char *control=(char *)s_control;
    int fd=-1;
    bool clean=false;
    if(!control) {
        ESP_LOGW(TAG,"No memory for the control buffer");
        atomic_store(&s.link,(int)LINK_DOWN);
        goto done;
    }
    fd=connect_server();
    // Anything below zero that is not the USB sentinel is a failed dial. The
    // two cases must be told apart, and `fd<0` alone does not do it: the USB
    // path returns a negative value by design, so the plain test discarded
    // every USB connection as a failure and reported "cannot connect" on a
    // cable that was working.
    if(fd<0) {
        // The error number is captured here rather than printed from the call,
        // where anything called in between is free to overwrite it and the
        // number reported would be some later failure's.
        int connect_errno=errno;
        ESP_LOGW(TAG,"RX connect failed errno=%d",connect_errno);
        // Conclusive: the dial is over and the server did not answer.
        atomic_store(&s.link,(int)LINK_DOWN);
        goto done;
    }
    // Reached only once the socket is up: from here the failure is the channel's,
    // not the server's.
    atomic_store(&s.link,(int)LINK_UP);
    s_rx_stage="hello";
    if (!hello(fd)) goto done;
    uint8_t wire[AV_HEADER_BYTES];
    unsigned header_deadline_ms=2000;
    int64_t last_header_us=0;
    uint32_t previous_phase[RX_PHASES]={0};
    while (!stopping()) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        atomic_store(&s.rx_cpu_us,own_cpu_us());
#endif
        s_rx_stage="header-read";
        memset(&h,0,sizeof(h));
        if (!io_all(fd,wire,sizeof(wire),false,header_deadline_ms)) break;
        s_rx_stage="header-decode";
        if(!av_header_decode(wire,&h)) {
            break;
        }
        // The gap is taken here, between two headers that both arrived: a
        // read that timed out ends the session, so it would otherwise be
        // recorded as one enormous gap that means nothing.
        {
            int64_t at=esp_timer_get_time();
            uint32_t parts[RX_PHASES];
            for(unsigned k=0;k<RX_PHASES;k++) {
                uint32_t total=atomic_load(&s.rx_timing[k].total_us);
                parts[k]=total-previous_phase[k];previous_phase[k]=total;
            }
            if (last_header_us) {
                uint32_t gap_ms=(uint32_t)((at-last_header_us)/1000);
                if (gap_ms>atomic_load(&s.rx_header_gap_max_ms))
                    atomic_store(&s.rx_header_gap_max_ms,gap_ms);
                taskENTER_CRITICAL(&clock_lock);
                uint32_t gap=(uint32_t)(at-last_header_us);
                if(gap>s.rx_stall_us) {
                    s.rx_stall_us=gap;
                    memcpy(s.rx_stall_parts,parts,sizeof(parts));
                }
                taskEXIT_CRITICAL(&clock_lock);
            }
            last_header_us=at;
        }
        s_rx_stage="stream-state";
        if(!av_stream_accept(&stream,&h)) break;
        s.last_packet_ms=(uint32_t)(esp_timer_get_time()/1000);
        if (h.type==AV_CONFIG || h.type==AV_ERROR) {
            if (!receive_config(fd,control,&h)) break;
            // Do NOT fix the playback origin here. A live server answers CONFIG
            // immediately and only then fills its transcode buffer, so anchoring
            // at CONFIG time would start the audio clock seconds before the
            // first sample arrives, and the queue would look starved on the
            // first read. The audio task sets the origin when it starts feeding.
            xEventGroupSetBits(s.events,CONFIG_READY);
            // The server answers the handshake immediately and only then fills
            // its transcode buffer, so the first media packet can be seconds
            // away. Relax the per-header deadline once, here. It is never
            // restored: this is a per-session local that a new session resets,
            // and a stalled stream is caught by the inter-packet watchdog in the
            // session loop, which is tighter than this deadline.
            // Do not "fix" it back to 2000 ms without rechecking that watchdog.
            header_deadline_ms=AV_FIRST_MEDIA_TIMEOUT_MS;
        } else if (h.type==AV_AUDIO) {
            audio_t a={.pts=h.pts_ms};
            atomic_store(&s.media_started,true);
            // Flow control, applied only to audio and only now that the header
            // is already read. The host sends audio at its own clock rate while
            // the device consumes at the I2S rate, and the two drift apart by a
            // fraction of a percent, so on a 20-chunk queue the drift eventually
            // overflows it whatever buffer the host keeps. Holding this packet
            // back stops us reading the socket and lets TCP push the host down
            // to the device's real rate.
            // It must not sit before the header read: the audio queue settles at
            // its ceiling, so a check there blocked almost every read and starved
            // video, which then arrived stale and was dropped.
            // Only STOP ends the wait: every audio-task exit path sets it.
            int64_t wait_start=esp_timer_get_time();
            while (!stopping() && uxQueueMessagesWaiting(s.audio)>PCM_QUEUE-4) {
                s_rx_stage="audio-flow-control";
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            uint32_t audio_wait=(uint32_t)(esp_timer_get_time()-wait_start);
            atomic_fetch_add(&s.rx_wait_us,audio_wait);
            av_rx_timing_add(&s.rx_timing[RX_AUDIO_QUEUE],audio_wait);
            if (stopping()) break;
            s_rx_stage="audio-read";
            int64_t io_start=esp_timer_get_time();
            bool audio_ok=io_all(fd,a.block,sizeof(a.block),false,AV_READ_DEADLINE_MS);
            atomic_fetch_add(&s.rx_io_us,(uint32_t)(esp_timer_get_time()-io_start));
            atomic_fetch_add(&s.rx_iterations,1u);
            if (!audio_ok) break;
            // A header no encoder wrote is a stream that is not the one agreed
            // in CONFIG; ending the session here is the same rule a malformed
            // video table gets, and keeps garbage out of the decoder.
            s_rx_stage="audio-header";
            if (!av_adpcm_header_valid(a.block)) break;
            atomic_fetch_add(&s.rx_audio_packets,1u);
            if (!xQueueSend(s.audio,&a,0)) { fail("bounded PCM queue full"); break; }
            unsigned q=uxQueueMessagesWaiting(s.audio); if(q>s.audio_high) s.audio_high=q;
        } else if (h.type==AV_VIDEO) {
            // A packet is [first][count][u16 length * count][stripes]. The table
            // is read first, then each stripe goes straight into the ring, so no
            // buffer is ever sized for a whole packet. Stripes of one packet are
            // consecutive on the wire and nothing else may be read between them.
            const bool frame_start=!(h.flags&AV_VIDEO_CONTINUES);
            if(frame_start) atomic_fetch_add(&s.rx_frame_starts,1u);
            else atomic_fetch_add(&s.rx_stray_packets,1u);
            atomic_store(&s.media_started,true);
            s_rx_stage="video-table";
            int64_t io_start=esp_timer_get_time();
            int64_t wait_total=0;
            uint8_t table[2+2*AV_STRIPES_PER_PACKET];
            uint16_t len[AV_STRIPES_PER_PACKET];
            unsigned first=0,count=0;
            uint32_t total=0;
            if(h.length<2 || !io_all(fd,table,2,false,AV_READ_DEADLINE_MS)) break;
            first=table[0]; count=table[1];
            if(!count || count>AV_STRIPES_PER_PACKET || first>=AV_STRIPES || first+count>AV_STRIPES
               || h.length<2u+2u*count) break;
            if(!io_all(fd,table+2,2u*count,false,AV_READ_DEADLINE_MS)) break;
            for(unsigned i=0;i<count;i++) {
                len[i]=(uint16_t)(((unsigned)table[2+2*i]<<8)|table[3+2*i]);
                if(len[i]>AV_STRIPE_CMAX) goto done;
                total+=len[i];
            }
            // The lengths must account for the payload exactly, as av_video_decode
            // requires: a frame that does not add up is not the one that was sent.
            if(total!=h.length-(2u+2u*count)) break;
            if(frame_start) {
                // A truncated predecessor was never visible and owns no queue entry.
                if(pending.count && !discard_frame) {
                    av_frame_ring_abort(&s.frame_ring,pending.mark);
                    s.dropped++;
                }
                pending=(av_complete_frame_t){.pts=h.pts_ms,.mark=s.frame_ring.head};
                discard_frame=false;
                frame_receive_started=esp_timer_get_time();
            }
            if(!av_frame_packet_matches(&pending,h.pts_ms,first,count,frame_start)) {
                s_rx_stage="frame-stripe-order"; goto done;
            }
            for(unsigned n=0;n<count;n++) {
                video_t v={.length=len[n],.stripe=(uint8_t)(first+n),.pts=h.pts_ms,
                           .frame_start=frame_start && n==0,.packet_start=(n==0),
                           .packet_end=(n+1==count)};
                if(len[n] && !discard_frame) {
                    s_rx_stage="frame-buffer";
                    int64_t wait0=esp_timer_get_time(),give_up=wait0+AV_VIDEO_WAIT_US;
                    bool got=false;
                    while(!stopping()) {
                        if(av_frame_ring_alloc(&s.frame_ring,len[n],&v)) { got=true; break; }
                        // If older committed frames have drained, waiting cannot
                        // help: this uncommitted frame itself exceeds capacity.
                        if(atomic_load(&s.frame_ring.tail)==pending.mark ||
                           esp_timer_get_time()>=give_up) break;
                        vTaskDelay(1);
                    }
                    int64_t waited=esp_timer_get_time()-wait0;
                    wait_total+=waited;
                    atomic_fetch_add(&s.rx_wait_us,(uint32_t)waited);
                    av_rx_timing_add(&s.rx_timing[RX_FRAME_RING],(uint32_t)waited);
                    if(stopping()) goto done;
                    if(!got) {
                        // Never publish partial frames or block audio behind an
                        // impossible allocation. Reclaim only uncommitted bytes.
                        av_frame_ring_abort(&s.frame_ring,pending.mark);
                        discard_frame=true; s.dropped++;
                        atomic_fetch_add(&s.frame_overflow,1u);
                        atomic_fetch_add(&s.rx_nobuf_packets,1u);
                    }
                }
                if(len[n]) {
                    if(!discard_frame) {
                        s_rx_stage="frame-read";
                        uint32_t cursor=v.data_start,left=len[n];
                        while(left) {
                            uint8_t *part=NULL;
                            uint32_t chunk=av_frame_ring_span(&s.frame_ring,&cursor,left,&part);
                            if(!chunk || !io_all(fd,part,chunk,false,AV_READ_DEADLINE_MS)) goto done;
                            left-=chunk;
                        }
                    } else {
                        s_rx_stage="frame-discard";
                        uint8_t discard[256]; uint32_t left=len[n];
                        int64_t deadline=esp_timer_get_time()+1500000;
                        while(left && !stopping()) {
                            unsigned k=left<sizeof(discard)?left:sizeof(discard);
                            if(!io_until(fd,discard,k,false,deadline)) goto done;
                            left-=k;
                        }
                        if(stopping()) goto done;
                    }
                }
                if(!discard_frame) pending.stripes[pending.count]=v;
                pending.count++; pending.bytes+=len[n];
            }
            if(av_frame_complete(&pending)) {
                frame_max(&s.frame_size_max,pending.bytes);
                if(!discard_frame) {
                    if(!video_enqueue(&pending)) goto done;
                    atomic_fetch_add(&s.frame_ready,1u);
                    frame_max(&s.frame_receive_max_us,(uint32_t)(esp_timer_get_time()-frame_receive_started));
                }
                pending.count=0;
            }
            atomic_fetch_add(&s.rx_io_us,(uint32_t)(esp_timer_get_time()-io_start-wait_total));
            atomic_fetch_add(&s.rx_iterations,1u);
            atomic_fetch_add(&s.rx_video_packets,1u);
            atomic_fetch_add(&s.rx_video_bytes,h.length);
            unsigned q=uxQueueMessagesWaiting(s.video); if(q>s.video_high) s.video_high=q;
        } else if (h.type==AV_PALETTE) {
            // Read straight into the session's palette: it is the only place
            // these bytes go, and they are already in the order the expander
            // wants. The header was validated, so the length is exactly
            // AV_PALETTE_BYTES and no second length check is needed here.
            uint8_t raw[AV_PALETTE_BYTES];
            s_rx_stage="palette-read";
            if (!io_all(fd,raw,sizeof(raw),false,AV_READ_DEADLINE_MS)) break;
            av_palette_decode(raw,s.palette);
            av_palette_wire_order(s.palette);
            s.palette_ready=true;
        } else if (h.type==AV_END) { clean=true; break; }
    }
done:
    if(pending.count && !discard_frame) av_frame_ring_abort(&s.frame_ring,pending.mark);
    // Only a real socket is shut down and closed. AV_TRANSPORT_USB is a
    // negative sentinel, not a descriptor, and passing it to close() would
    // either fail harmlessly or -- worse -- close an unrelated descriptor,
    // because close takes whatever number it is given.
    if (fd>=0) { shutdown(fd,SHUT_RDWR); close(fd); }
    if (!clean) {
        ESP_LOGW(TAG,"RX_EXIT stage=%s cancelled=%d type=%u seq=%"PRIu32" expected_seq=%"PRIu32
            " pts=%"PRIu32" length=%"PRIu32" audio_next_pts=%"PRIu32" video_pts=%"PRIu32,
            s_rx_stage,stopping(),h.type,h.seq,stream.next_seq,h.pts_ms,h.length,
            stream.audio_next_pts,stream.video_pts);
        if(!stopping()) fail(s_rx_stage);
    }
    xEventGroupSetBits(s.events,RX_DONE);
    vTaskDelete(NULL);
}
static void audio_task(void *arg) {
    (void)arg;
    bool opened=false;
    while (!stopping() && !(xEventGroupGetBits(s.events)&CONFIG_READY)) vTaskDelay(pdMS_TO_TICKS(10));
    if (stopping()) goto done;
    if (bsp_audio_set_format(16000,16,1)!=ESP_OK) { fail("codec open"); goto done; }
    opened=true; bsp_audio_set_mute(true); bsp_audio_set_volume(s.volume);
    // Clear any prior-session DMA audio while muted. >90ms, finite writes.
    audio_t a={0};
    // What the codec is fed. The queue holds ADPCM; this is the one chunk of it
    // that has been expanded, and it is also the silence buffer.
    int16_t pcm[AV_AUDIO_SAMPLES]={0};
    for (int i=0;i<6 && !stopping();i++) {
        size_t written=0;
        if (bsp_audio_write_timeout(pcm,sizeof(pcm),&written,100)!=ESP_OK || written!=sizeof(pcm)) {
            fail("I2S reset write"); goto done;
        }
    }
    // Allow the final silence to drain, then wait for a reserve of audio. This
    // also covers the gap between CONFIG and the server's first media packet,
    // which can be seconds on a live channel.
    if (!delay_until(esp_timer_get_time()+100000)) goto done;
    int64_t prebuffer_deadline=esp_timer_get_time()+AV_FIRST_MEDIA_TIMEOUT_MS*1000;
    while (!stopping() && uxQueueMessagesWaiting(s.audio)<5 && !rx_done()
           && esp_timer_get_time()<prebuffer_deadline) vTaskDelay(pdMS_TO_TICKS(5));
    if (stopping()) goto done;
    if (uxQueueMessagesWaiting(s.audio)<5) { fail("no audio after CONFIG"); goto done; }
    // The origin is anchored here, at the first real feed, not at CONFIG.
    if (!delay_until(esp_timer_get_time()+DMA_ESTIMATE_MS*1000)) goto done;
    // The codec stays muted until there is real audio in hand.
    //
    // Previously this unmuted before the loop below, on the strength of the
    // queue check a few lines up. That check is a floor, not a guarantee: the
    // first receive can still miss and fall into the empty-queue wait, leaving
    // the output enabled for up to AUDIO_UNDERRUN_MS with nothing to play. An
    // enabled output with a silent input is audible as hiss, so opening it at the
    // moment the first block is actually written keeps it on only while there is
    // something to put through it.
    bool unmuted=false;
    int64_t last_feed=0;
    uint8_t volume=s.volume;
    // When the queue last ran dry, or zero while the sound is being served.
    // Cleared by the first real chunk, and it is what the silence budget below
    // is measured against.
    int64_t starved_since=0;
    bool starved_reported=false;
    // Whether the chunk about to be written is silence this device made up
    // rather than sound that arrived. It decides which counter the write is
    // charged to; see silence_samples.
    bool silent=false;
    while (!stopping()) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        atomic_store(&s.audio_cpu_us,own_cpu_us());
#endif
        if (!xQueueReceive(s.audio,&a,pdMS_TO_TICKS(AV_AUDIO_MS))) {
            // An empty queue is not yet a fault, and it is not silence either:
            // the I2S DMA still holds up to ~90 ms and Wi-Fi delivers in bursts.
            //
            // What it used to be is fatal. Failing here is what produced the
            // viewer's "most channels turn into colour bars after a while":
            // the queue is emptied by the *picture*, because one task reads both
            // streams, and a picture packet in flight measured 612 ms against
            // this 300 ms tolerance. The sound was never broken; it was starved
            // by the picture and reported as a fault.
            //
            // So play silence instead, and carry on. The picture drops a frame
            // under the same strain rather than tearing the session down, and
            // the sound now behaves the same way. Nothing is faked: the clock
            // below advances by the sample count actually submitted, so the
            // timeline reflects silence that really was played, and the picture
            // -- whose timestamps follow the sound's -- stays with it.
            if (stopping() || rx_done()) break;
            if (!starved_since) starved_since=esp_timer_get_time();
            int64_t starved_ms=(esp_timer_get_time()-starved_since)/1000;
            // Reported once per episode rather than once per chunk: an episode
            // is a run of silent chunks, and logging each would put fifteen
            // lines in the log for one picture packet at 115200 baud.
            if (starved_ms>=AUDIO_UNDERRUN_MS && !starved_reported) {
                starved_reported=true;
                ESP_LOGW(TAG,"AUDIO_EMPTY gap_ms=%"PRId64" queue=0 submitted=%"PRIu32
                    " -- playing silence, session kept",
                    starved_ms,(uint32_t)s.submitted_samples);
            }
            // A source that has genuinely gone quiet still has to be noticed,
            // or the screen would sit on a still frame for ever. The receiver's
            // own inactivity timeout catches that case; this is the backstop
            // for a stream that keeps sending picture and no sound at all.
            if (starved_ms>AUDIO_SILENCE_MAX_MS) {
                ESP_LOGW(TAG,"AUDIO_STARVED gap_ms=%"PRId64" -- giving up",
                    starved_ms);
                fail("audio starved beyond budget; reconnect/rebuffer"); break;
            }
            // The chunk that is fed instead of a real one. `pcm` still holds the
            // last one that played, so it is zeroed rather than left as it was
            // -- feeding the previous chunk again would repeat 40 ms of sound.
            memset(pcm,0,sizeof(pcm));
            silent=true;
        } else {
            starved_since=0;
            starved_reported=false;
            silent=false;
            // Each block carries its own decoder state, so no state is kept
            // from one chunk to the next, and a stretch of silence in between
            // leaves nothing for the next block to inherit.
            av_adpcm_decode(a.block,AV_AUDIO_SAMPLES,pcm);
        }
        if (!unmuted) {
            unmuted=true;
            bsp_audio_set_mute(false);
        }
        if (volume!=s.volume) { volume=s.volume; bsp_audio_set_volume(volume); }
        int64_t before=esp_timer_get_time();
        if (!last_feed) {
            taskENTER_CRITICAL(&clock_lock);
            s.clock_us=before+DMA_ESTIMATE_MS*1000;
            taskEXIT_CRITICAL(&clock_lock);
            xEventGroupSetBits(s.events,CLOCK_READY);
        } else {
            unsigned gap=(before-last_feed)/1000;
            if (gap>s.feed_gap_max_ms) s.feed_gap_max_ms=gap;
            // One chunk more than the queue-empty check above, because this
            // interval covers one extra 20 ms write. The previous 80 ms limit
            // was below the DMA's own ~90 ms of cover, so a routine Wi-Fi gap
            // failed a session that was still playing audio.
            if (gap>AUDIO_FEED_GAP_MS) { fail("audio feed gap exceeds DMA budget"); break; }
        }
        size_t written=0;
        esp_err_t e=bsp_audio_write_timeout(pcm,sizeof(pcm),&written,100);
        taskENTER_CRITICAL(&clock_lock);
        s.submitted_samples+=written/2;
        // Charged apart. This write went to the codec and really did play, so
        // it counts as submitted; but no packet ever carried it, so it must
        // not count as program progress. Counting it in both is what made the
        // picture permanently late after a gap.
        if(silent) s.silence_samples+=written/2;
        taskEXIT_CRITICAL(&clock_lock);
        last_feed=esp_timer_get_time();
        if (e!=ESP_OK || written!=sizeof(pcm)) { fail("I2S timeout/partial write"); break; }
    }
    if (!stopping()) delay_until(esp_timer_get_time()+100000); // Estimated final drain.
done:
    if (opened) {
        bsp_audio_set_mute(true);
        if (bsp_audio_stream_close()!=ESP_OK) fail("codec close");
    }
    ESP_LOGD(TAG,"audio task stack high-water=%u bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xEventGroupSetBits(s.events,AUDIO_DONE);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Overlay. The panel is owned through the raw path and the BSP refuses to start
// LVGL once raw has claimed it, so the overlay is drawn straight into the stripe
// buffers on their way to the panel. That also means it costs no extra memory:
// the stripes already exist and are already the right size.
// ---------------------------------------------------------------------------

// Menu geometry. Both the drawing code and the scroll logic need the same
// numbers: computing the row count only where it was drawn left the menu's own
// idea of how many rows fit stuck at zero, so the list never scrolled.
#define MENU_HEADER_H 26
#define MENU_ROW_H    24

// How many list rows fit under the header.
static ui_menu_overlay_t menu_overlay;

static unsigned menu_rows(void) {
    return UI_MENU_OVERLAY_ROWS;
}

// Rows of the panel a stripe covers, as a drawing surface.
static ui_surface_t stripe_surface(unsigned stripe_y, uint8_t *pixels) {
    ui_surface_t surface = { pixels, (int)AV_WIDTH, (int)stripe_y, (int)AV_STRIPE_ROWS };
    return surface;
}

// Copy the display name for a channel id, or the id itself when there is no
// name, into the caller's buffer. Used for the "connecting to" banner, which
// names a channel that is not the one playing yet, so it cannot go through
// current_channel_name().
//
// The name is copied out under the lock, never returned as a pointer into the
// table: the receive task replaces that table from CONFIG, and a pointer handed
// back here was dereferenced later by the caller, on another task, with no lock
// held -- a read of ids and names that could be rewritten mid-use.
static void channel_name_of(const char *id, char *out, size_t size) {
    if (!out || !size) return;
    out[0]='\0';
    if (!id || !id[0]) return;
    bool found=false;
    taskENTER_CRITICAL(&list_lock);
    unsigned count=atomic_load(&s.count);
    for (unsigned i=0;i<count;i++) {
        if (!strcmp(s.list[i].id,id)) {
            snprintf(out,size,"%s",s.list[i].name[0] ? s.list[i].name : s.list[i].id);
            found=true;
            break;
        }
    }
    taskEXIT_CRITICAL(&list_lock);
    // Not in the list: the id is still the most useful thing to show, and it is
    // the caller's own string rather than anything this task is holding.
    if (!found) snprintf(out,size,"%s",id);
}

// The name to show for the channel now playing, copied out under the lock for
// the same reason as above. Falls back to the id when the server sent no
// display name, so the banner is never blank.
static void current_channel_name(char *out, size_t size) {
    if (!out || !size) return;
    out[0]='\0';
    bool found=false;
    taskENTER_CRITICAL(&list_lock);
    unsigned count=atomic_load(&s.count);
    for (unsigned i=0;i<count;i++) {
        if (!strcmp(s.list[i].id,s.channel)) {
            snprintf(out,size,"%s",s.list[i].name[0] ? s.list[i].name : s.list[i].id);
            found=true;
            break;
        }
    }
    taskEXIT_CRITICAL(&list_lock);
    if (!found) snprintf(out,size,"%s",s.channel[0] ? s.channel : "?");
}

// Signal bars from the connected access point's RSSI. Returns 0..4; 0 means the
// reading failed, which the status page shows as "no signal" rather than as a
// weak one, because an unreadable value is not the same as a bad one.
// The raw reading, for the interval log. wifi_bars() below is the display's
// coarser view of the same thing; this is what a session that is starving for
// bandwidth needs, because 0..4 bars cannot tell a marginal link from a good one.
static int wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return 0;
    return ap.rssi;
}

// Which of 11b/11g/11n the access point offered, as a short string, for the
// interval log. A good signal is not the same as a fast one: an access point
// answering only in 11b gives a strong reading and about a tenth of the
// throughput, and every measurement taken here would look like the device being
// slow rather than the link being narrow.
static const char *wifi_phy(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return "?";
    if (ap.phy_11n && ap.phy_11g && ap.phy_11b) return "bgn";
    if (ap.phy_11n) return "n";
    if (ap.phy_11g) return "g";
    if (ap.phy_11b) return "b";
    return "-";
}

static unsigned wifi_channel(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return 0;
    return ap.primary;
}

static unsigned wifi_bars(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return 0;
    int rssi=ap.rssi;
    if (rssi>=-55) return 4;
    if (rssi>=-65) return 3;
    if (rssi>=-75) return 2;
    return 1;
}

static void drain_display(void);

// Readings for the black bars around the picture, drawn into whichever stripe
// overlaps a bar and nothing else. The picture covers rows
// [AV_VIDEO_Y_OFFSET, AV_HEIGHT-AV_VIDEO_Y_OFFSET), so these rows belong to the
// overlay alone: WiFi and battery in the top bar, frame rate and receive rate in
// the bottom one. A bar too thin to hold a line of text is left alone.
static void info_bars_stripe(ui_surface_t surface) {
    // Text sits 16 px from each edge: the panel's corners are rounded, and the
    // bars are exactly where they cut in.
    const int edge=UI_TEXT_PAD_X+2+8;   // half a 16 px glyph
    const int bar=(int)AV_VIDEO_Y_OFFSET;
    const int box_h=(int)UI_TEXT_LINE_H+2*UI_TEXT_PAD_Y;
    if (bar<box_h) return;
    const int first=surface.origin_y, last=surface.origin_y+surface.rows;
    const int pad=(bar-(int)UI_TEXT_LINE_H)/2;
    char text[24];

    const int top_y=pad;
    if (first<bar && last>top_y-UI_TEXT_PAD_Y) {
        const int center_y=top_y+(int)UI_TEXT_LINE_H/2;
        ui_status_wifi(surface,edge,center_y-UI_STATUS_WIFI_H/2,wifi_bars(),UI_COLOR_BOX);
        ui_status_battery(surface,(int)AV_WIDTH-edge,center_y,
                          atomic_load(&s.battery_soc),UI_COLOR_BOX);
    }

    const int bottom=(int)AV_HEIGHT-bar;
    const int bottom_y=bottom+pad;
    if (last>bottom) {
        // Cleared first: the list's last row can run into this band on a screen
        // that paints the whole height, and it must not show through.
        ui_text_fill(surface,0,bottom,(int)AV_WIDTH,bar,UI_COLOR_BOX);
        uint32_t fps=atomic_load(&s.fps_x10);
        snprintf(text,sizeof(text),"FPS %u.%u",(unsigned)(fps/10u),(unsigned)(fps%10u));
        ui_text_draw(surface,edge,bottom_y,text,UI_COLOR_DIM,UI_COLOR_BOX);
        snprintf(text,sizeof(text),"Net %u KB/s",
                 (unsigned)((atomic_load(&s.rx_bytes_per_s)+512u)/1024u));
        ui_text_draw(surface,(int)AV_WIDTH-edge-ui_text_width(text),bottom_y,
                     text,UI_COLOR_DIM,UI_COLOR_BOX);
    }
}

// Draw menu readings on the bars, or blank them, between frames.
// Called from the video task, which owns the panel: the queue is drained first so
// the stripe buffer is free, then each band is built in s.stripe[0] and sent the
// way the first frame blackens the bars. Does nothing until the picture owns the
// panel; before that the waiting screen paints the bars itself.
static bool refresh_info_bars(void) {
    if (!atomic_load(&s.media_started)) return true;
    const bool want=s.menu.view==UI_VIEW_MENU || s.menu.view==UI_VIEW_CONTROLS ||
                    s.menu.view==UI_VIEW_QUALITY;
    const int64_t now=esp_timer_get_time();
    if (want) {
        if (bars_drawn && now-bars_painted_us<1000000) return true;
    } else if (!bars_drawn) {
        return true;
    }
    const unsigned bar=(unsigned)AV_VIDEO_Y_OFFSET;
    if (bar==0) return true;
    drain_display();
    const unsigned starts[2]={0u,(unsigned)AV_HEIGHT-bar};
    for (unsigned b=0;b<2;b++) {
        for (unsigned y=starts[b];y<starts[b]+bar;y+=AV_STRIPE_ROWS) {
            unsigned rows=starts[b]+bar-y;
            if (rows>AV_STRIPE_ROWS) rows=AV_STRIPE_ROWS;
            memset(s.stripe[0],0,STRIPE_BYTES);
            if (want) info_bars_stripe(stripe_surface(y,s.stripe[0]));
            if (bsp_display_raw_submit_nowait(y,rows,s.stripe[0])!=ESP_OK ||
                bsp_display_raw_drain(1,200)!=ESP_OK) {
                fail("info bar DMA");
                return false;
            }
        }
    }
    bars_drawn=want;
    bars_painted_us=now;
    return true;
}

// The status page's text, rebuilt at most once a second and read by every
// stripe. Only the video task touches it: both callers of overlay_stripe run
// there.
#define STATUS_LINES 4u
static char status_text[STATUS_LINES][UI_MENU_NAME_MAX+24];
static int64_t status_text_us;
static bool status_text_valid;

static void refresh_status_text(void) {
    const int64_t now=esp_timer_get_time();
    if (status_text_valid && now-status_text_us<1000000) return;
    status_text_valid=true;
    status_text_us=now;

    // Copied out under the table lock: the receive task may be replacing the
    // table while this runs.
    char name[UI_MENU_NAME_MAX];
    current_channel_name(name,sizeof(name));
    snprintf(status_text[0],sizeof(status_text[0]),"Channel  %s",name);

    unsigned bars=wifi_bars();
    if (bars) snprintf(status_text[1],sizeof(status_text[1]),"Signal  %u/4",bars);
    else snprintf(status_text[1],sizeof(status_text[1]),"Signal  --");

    int soc=atomic_load(&s.battery_soc);
    if (soc>=0) snprintf(status_text[2],sizeof(status_text[2]),"Battery  %d%%",soc);
    else snprintf(status_text[2],sizeof(status_text[2]),"Battery  --");

    // It says "WiFi" rather than "network" because that is the word on the
    // setup page the viewer will land on, and it says which arrow because the
    // other one does nothing here.
    snprintf(status_text[3],sizeof(status_text[3]),"Hold OK to exit TV");
}

// Draw one stripe's worth of overlay, if this stripe is one the overlay occupies.
// Everything is positioned from absolute screen rows so a stripe can be drawn in
// isolation without knowing what came before it.
static void overlay_stripe(unsigned stripe_y, uint8_t *pixels) {
    ui_surface_t surface=stripe_surface(stripe_y,pixels);
    if (menu_overlay.view==UI_VIEW_MENU || menu_overlay.view==UI_VIEW_QUALITY || menu_overlay.view==UI_VIEW_CONTROLS) {
        ui_menu_overlay_draw(&menu_overlay,surface);
        return;
    }
    unsigned first=stripe_y, last=stripe_y+AV_STRIPE_ROWS;   // [first,last)

    // A helper for anchored drawing: place a line whose text top is `y`, only
    // when this stripe overlaps the rows the text and its box occupy.
    #define LINE_TOP_PAD UI_TEXT_PAD_Y
    #define LINE_HEIGHT ((int)UI_TEXT_LINE_H + 2*UI_TEXT_PAD_Y)

    if (s.menu.view==UI_VIEW_VIDEO) {
        // The volume indicator, at the top of the screen while it is up.
        //
        // The top, not the bottom, because the channel banner already lives at
        // the bottom and the two can be on screen together -- change channel,
        // then reach for the volume before the banner has expired. Putting this
        // above it would have hidden one of them, and neither is worth losing.
        //
        // It shows a number and a filled bar together: the number is the exact
        // level, the bar is how much of the range is left, and a glance usually
        // wants the second.
        if (ui_menu_volume_visible(&s.menu)) {
            // Laid out downwards from the text, not from a guessed band height:
            // ui_text_draw paints a box of LINE_HEIGHT rows, and the bar has to
            // start below that box's bottom edge or it draws over the glyphs.
            // The band is then whatever that comes to.
            const int box_top=(int)AV_VIDEO_Y_OFFSET;
            const int text_bottom=box_top+LINE_TOP_PAD+LINE_HEIGHT;
            const int bar_y=text_bottom+4, bar_h=4;
            const int box_h=(bar_y+bar_h)-box_top;
            if ((int)first<(unsigned)(box_top+box_h) && (int)last>(unsigned)box_top) {
                char text[20];
                snprintf(text,sizeof(text),"Volume %u%%",s.menu.volume);
                // One block first, so the text and the bar sit on the same
                // known colour rather than directly on the picture.
                ui_text_fill(surface,0,box_top,(int)AV_WIDTH,box_h,UI_COLOR_BOX);
                ui_text_draw(surface,12,box_top+LINE_TOP_PAD,text,UI_COLOR_TEXT,UI_COLOR_BOX);
                const int bar_x=12;
                const int bar_w=(int)AV_WIDTH-2*bar_x;
                ui_text_fill(surface,bar_x,bar_y,bar_w,4,UI_COLOR_RULE);
                const int filled=(int)((unsigned)bar_w*s.menu.volume/100u);
                if (filled>0) ui_text_fill(surface,bar_x,bar_y,filled,4,UI_COLOR_SELECT);
            }
        }
        // The banner sits at the bottom, just above the edge: it is the least
        // disruptive place for something that appears on its own.
        if (s.menu.banner_ms>0 && s.menu.banner[0]) {
            int height=LINE_HEIGHT, top=(int)(AV_HEIGHT-AV_VIDEO_Y_OFFSET)-height;
            if ((int)first<(int)(AV_HEIGHT-AV_VIDEO_Y_OFFSET) && (int)last>top) {
                // A full-width bar reads as a caption rather than a stray box.
                ui_text_fill(surface,0,top,(int)AV_WIDTH,height,UI_COLOR_BOX);
                int width=ui_text_width(s.menu.banner);
                int x=(width>=(int)AV_WIDTH)?0:((int)AV_WIDTH-width)/2;
                ui_text_draw(surface,x,top+LINE_TOP_PAD,s.menu.banner,
                             UI_COLOR_TEXT,UI_COLOR_BOX);
            }
        }
    } else if (s.menu.view==UI_VIEW_MENU || s.menu.view==UI_VIEW_QUALITY) {
        return; // The next complete frame freezes the newly opened overlay.
    } else if (s.menu.view==UI_VIEW_BRIGHTNESS) {
        // The backlight page. Deliberately the same shape as the volume
        // indicator: a labelled level over a filled bar, because they are the
        // same kind of thing and should not need to be learned twice.
        //
        // It is drawn on the video rather than as a list row, so it does not
        // depend on a channel list having arrived: the backlight has to be
        // reachable on a device that has not connected to a server yet.
        const int header=MENU_HEADER_H;
        if (first<(unsigned)header) {
            ui_text_fill(surface,0,0,(int)AV_WIDTH,header,UI_COLOR_BOX);
        }
        if (first<(unsigned)header+1u && last>(unsigned)header) {
            ui_text_fill(surface,0,header,(int)AV_WIDTH,1,UI_COLOR_RULE);
        }
        const int level_y=header+16;
        if ((int)last>level_y-UI_TEXT_PAD_Y && (int)first<level_y+LINE_HEIGHT) {
            char text[32];
            snprintf(text,sizeof(text),"Brightness  %u%%",
                     av_brightness_percent(s.menu.brightness));
            ui_text_draw(surface,14,level_y,text,UI_COLOR_TEXT,UI_COLOR_BOX);
            // The bar sits below the label, laid out from the label's own box so
            // it cannot be drawn over the glyphs.
            const int bar_x=14, bar_y=level_y+LINE_HEIGHT, bar_h=6;
            const int bar_w=(int)AV_WIDTH-2*bar_x;
            ui_text_fill(surface,bar_x,bar_y,bar_w,bar_h,UI_COLOR_RULE);
            // Filled from the level itself, so the bar shows where this step
            // sits in the range rather than which step number it is.
            const unsigned percent=av_brightness_percent(s.menu.brightness);
            const int filled=(int)((unsigned)bar_w*percent/100u);
            if (filled>0) ui_text_fill(surface,bar_x,bar_y,filled,bar_h,UI_COLOR_SELECT);
            // The hint, dimmed, one line under the bar.
            const int hint_y=bar_y+bar_h+10;
            if ((int)last>hint_y-UI_TEXT_PAD_Y && (int)first<hint_y+LINE_HEIGHT) {
                ui_text_draw(surface,14,hint_y,"Up/Down adjust, OK done",
                             UI_COLOR_DIM,UI_COLOR_BACKDROP);
            }
        }
    } else {
        // Status page: three labelled lines, drawn straight onto the backdrop.
        const int header=MENU_HEADER_H, line=MENU_ROW_H;
        if (first<(unsigned)header) {
            ui_text_fill(surface,0,0,(int)AV_WIDTH,header,UI_COLOR_BOX);
        }
        if (first<(unsigned)header+1u && last>(unsigned)header) {
            ui_text_fill(surface,0,header,(int)AV_WIDTH,1,UI_COLOR_RULE);
        }
        // Four lines: three readings and the gesture that leaves for setup. The
        // last is not decoration -- the setup gesture is a long press on an
        // arrow, which nothing else on this page uses and which nobody would
        // guess, so the page has to say it.
        //
        // The text is built once a second by refresh_status_text() and only
        // drawn here. This runs for every stripe of every frame, and building
        // the lines in place meant a WiFi driver call and a critical section
        // several times a frame.
        refresh_status_text();
        for (unsigned i=0;i<STATUS_LINES;i++) {
            int y=header+8+(int)i*line;
            if ((int)last<=y-UI_TEXT_PAD_Y || (int)first>=y+(UI_TEXT_LINE_H+UI_TEXT_PAD_Y)) continue;
            // The instruction is dimmed, because it is not a reading.
            ui_text_draw(surface,14,y,status_text[i],
                         i==STATUS_LINES-1u?UI_COLOR_DIM:UI_COLOR_TEXT,UI_COLOR_BACKDROP);
        }
    }
    #undef LINE_HEIGHT
    #undef LINE_TOP_PAD
}

// Inflating one stripe, then expanding it in place, then handing it to the
// panel: the three steps that replaced a whole JPEG decoder.
//
// The expansion is what makes one buffer do the work of two. The index bytes
// land in the first AV_STRIPE_PIXELS of the stripe buffer and are replaced,
// from the back forwards, by the RGB565 that the panel reads -- so the buffer
// written to is the buffer sent, and nothing the size of a frame is ever held.
// See av_expand_indexed for why running backwards is the only safe direction.
//
// Returns false on anything malformed. A stripe that does not decompress to
// exactly its expected size leaves the tail of the buffer holding the previous
// stripe's colours, which would look like a torn picture rather than an error,
// so the size is checked rather than trusted.
// --- One-shot screen capture ---------------------------------------------
//
// The only way to see what this device actually hands the panel, and it exists
// because every other check of the colour path compares the server's own
// output against itself. Those checks cannot see a fault on this side of the
// wire, and the screen cannot be read from a distance, so a colour reported as
// wrong had no way to be confirmed or located.
//
// One frame's worth of what was submitted, subsampled to every fourth column
// and every fourth row -- 80x60 out of 320x240, enough to judge a cast or a
// channel swap, a tenth of the bytes. Kept as RGB565 big-endian, byte for byte
// as the stripe held it, so what comes back is the panel's input rather than a
// second derivation of it that could be wrong in the same way.
#define SHOT_COLS 80u
#define SHOT_ROWS 60u
#define SHOT_BYTES (SHOT_COLS * SHOT_ROWS * 2u)
// Frames to draw before taking the shot. Past the first seconds of a session,
// so the picture is a real one rather than whatever followed a channel change.
#define SHOT_AFTER_FRAMES 60u

static uint8_t *s_shot;
static bool s_shot_wanted, s_shot_done, s_shot_taking;
static unsigned s_shot_rows, s_shot_frames;
// Static rather than on the stack: this runs on the video task, whose stack is
// sized for decoding and has no room to spare for a line of text. Four hex
// characters a pixel and a little room over, so the line is never truncated --
// a truncated line would silently lose the right-hand end of the picture.
static char s_shot_line[SHOT_COLS * 4u + 16u];

static void shot_take(unsigned y, const uint8_t *rgb565) {
    for (unsigned r = 0; r < AV_STRIPE_ROWS && s_shot_rows < SHOT_ROWS; r++) {
        if ((y + r) % 4u) continue;
        const uint8_t *src = rgb565 + (size_t)r * AV_WIDTH * 2u;
        uint8_t *dst = s_shot + (size_t)s_shot_rows * SHOT_COLS * 2u;
        for (unsigned c = 0; c < SHOT_COLS; c++) {
            dst[2 * c]     = src[4 * c * 2];
            dst[2 * c + 1] = src[4 * c * 2 + 1];
        }
        s_shot_rows++;
    }
}

// Plain hex over the console, one row of pixels a line, between two markers.
// Deliberately not the logging system: a line here is 168 characters and the
// logger truncates, which would silently lose the right-hand end of the frame.
static void shot_print(void) {
    printf("SHOT begin %u %u\n", (unsigned)SHOT_COLS, (unsigned)SHOT_ROWS);
    for (unsigned r = 0; r < SHOT_ROWS; r++) {
        int n = snprintf(s_shot_line, sizeof(s_shot_line), "SHOT");
        for (unsigned c = 0; c < SHOT_COLS; c++) {
            const uint8_t *p = s_shot + ((size_t)r * SHOT_COLS + c) * 2u;
            n += snprintf(s_shot_line + n, sizeof(s_shot_line) - n,
                          "%02x%02x", p[0], p[1]);
        }
        printf("%s\n", s_shot_line);
    }
    printf("SHOT end\n");
    free(s_shot); s_shot = NULL;
    s_shot_wanted = false;
    s_shot_done = true;
}

// Ask for a capture, when this build takes them.
//
// Called once the session's buffers exist: the picture has to have somewhere to
// come from, and the request is deliberately not tied to a button -- see the
// note in process_key.
static void shot_maybe_request(void) {
}

// Enlarge one stripe from the size it arrived at to the panel's own.
//
// Kept as its own pass after being merged into the expansion and measured.
// The merged version is exact -- checked pixel for pixel against this one --
// and it writes 153600 fewer bytes a frame, and it is still slower: the
// device's own `decode_max_ms` went from 78 to 91 ms, and turning the inner
// division into a multiply changed nothing (90). The reason is the lookups:
// this order does 43200 palette reads and then moves bytes, while looking up on
// the way out does 76800, one per panel pixel, and a table read on this chip
// costs more than the byte moves it saves.
//
// At the ratio in force this does nothing; the sizes in av_protocol.h are
// checked against AV_ENLARGE_NUM and AV_ENLARGE_DEN by static assertions, so a
// geometry change that needs enlarging gets it without this file being
// rewritten, and a change that does not gets the no-op branch.
//
// Nearest neighbour, and the ratio is an exact fraction, so the copy is exact:
// output (x,y) is a copy of input (x*DEN/NUM, y*DEN/NUM) with no interpolation
// and no rounding. That is the whole reason the arrival size is a fixed ratio of
// the panel's -- at any ratio that is not a simple fraction the device would
// have to filter neighbouring pixels together, which costs far more than the
// enlargement saves and softens the picture in the process.
//
// Backwards in both directions: every write lands at or above the input byte it
// reads, so one buffer serves as both source and destination.
static void enlarge_stripe(uint8_t *buf) {
#if AV_ENLARGE_NUM == AV_ENLARGE_DEN
    (void)buf;
#else
    for (int y=(int)AV_STRIPE_ROWS-1; y>=0; y--) {
        const unsigned source_y=(unsigned)y*AV_ENLARGE_DEN/AV_ENLARGE_NUM;
        const uint8_t *src=buf+(size_t)source_y*AV_VIDEO_WIDTH*2u;
        uint8_t *dst=buf+(size_t)y*AV_WIDTH*2u;
        for (int x=(int)AV_WIDTH-1; x>=0; x--) {
            const unsigned source_x=(unsigned)x*AV_ENLARGE_DEN/AV_ENLARGE_NUM;
            dst[2*x]=src[2*source_x];
            dst[2*x+1]=src[2*source_x+1];
        }
    }
#endif
}
// The four-pixels-a-step expander is written for whole groups on a 4-byte
// boundary; the heap gives that for a DMA-capable buffer and this pins the count.
_Static_assert(AV_STRIPE_PIXELS%4u==0u,"stripe pixel count must be a multiple of four");
static tinfl_status inflate_frame_stripe(const video_t *stripe,uint8_t *buf,
                                         size_t *input_bytes,size_t *output_bytes) {
    size_t produced=0,consumed=0;
    uint32_t cursor=stripe->data_start,left=stripe->length;
    tinfl_init(s.inflate);
    tinfl_status status=TINFL_STATUS_NEEDS_MORE_INPUT;
    while(left) {
        uint8_t *source=NULL;
        uint32_t chunk=av_frame_ring_span(&s.frame_ring,&cursor,left,&source);
        if(!chunk)break;
        size_t used=chunk,made=AV_STRIPE_PIXELS-produced;
        unsigned flags=TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if(left>chunk)flags|=TINFL_FLAG_HAS_MORE_INPUT;
        status=tinfl_decompress(s.inflate,source,&used,buf,buf+produced,&made,flags);
        consumed+=used;produced+=made;
        if(used>chunk || produced>AV_STRIPE_PIXELS)break;
        left-=(uint32_t)used;
        if(status==TINFL_STATUS_DONE)break;
        if(status!=TINFL_STATUS_NEEDS_MORE_INPUT || used!=chunk)break;
    }
    *input_bytes=consumed;*output_bytes=produced;return status;

}
static bool push_stripe(const video_t *stripe, unsigned y) {
    size_t available=stripe->length;unsigned n=stripe->stripe;
    // A stripe of length zero is one the sender chose not to resend because the
    // panel already shows it (TV_DELTA on the server). Nothing to inflate and
    // nothing to put on the bus; it is not an error.
    if(!available) return true;
    // Take the next buffer in turn, and wait for its previous transfer only
    // now -- here, where it is about to be written over. The panel has had the
    // whole of the last stripe's build time to finish sending it, so this
    // normally returns at once, and when it does not the wait is real work not
    // yet finished rather than a processor sitting idle beside a DMA engine.
    // Whichever buffer is free. With two, the one built here is not the one
    // the panel is reading, and the drain below retires the stripe before it.
    uint8_t *buf=s.stripe[s.stripe_next];
    s.stripe_next^=1u;
    // Receive every complete frame as before. When the frozen opaque sheet
    // replaces every pixel in a stripe, its hidden video needs no inflate or
    // palette expansion. The rounded edges and visible video are unchanged.
    bool covered=false;
#if AV_WIDTH == 320u && AV_ENLARGE_NUM == AV_ENLARGE_DEN
    covered=ui_menu_overlay_covers(&menu_overlay,y,AV_STRIPE_ROWS);
#endif
    atomic_fetch_add(&s.inflate_bytes,(uint32_t)available);
    if (!covered) {
    size_t produced=0,consumed=0;
    int64_t inflate_start=esp_timer_get_time();
    tinfl_status status=inflate_frame_stripe(stripe,buf,&consumed,&produced);
    // Relay-ordered, like the byte counters beside them. These are written by
    // the video task and read by the session loop, and a plain += on an
    // atomic_uint_least32_t is a read-modify-write that need not be atomic:
    // two concurrent increments can lose one. These are statistics, so a lost
    // count would misreport the very thing they were added to measure.
    atomic_fetch_add(&s.inflate_us,(uint32_t)(esp_timer_get_time()-inflate_start));
    // consumed counts bytes taken; all of them have to go, or the stripe is not
    // the one the server cut.
    if(status!=TINFL_STATUS_DONE || consumed!=available || produced!=AV_STRIPE_PIXELS) {
        ESP_LOGW(TAG,"Stripe %u: status=%d consumed=%u/%u produced=%u/%u",
                 n,(int)status,(unsigned)consumed,(unsigned)available,
                 (unsigned)produced,(unsigned)AV_STRIPE_PIXELS);
        return false;
    }
    // Two passes, and the one-pass version that replaced them was measured and
    // put back.
    //
    // The reasoning for one pass was sound: enlarging is a copy of one pixel to
    // another position and expanding is a lookup, so the two commute, and
    // "look up and write where the copy would have gone" avoids writing 153600
    // bytes a frame that the expansion had only just written. It was verified
    // pixel for pixel against the two-pass result and matched exactly.
    //
    // It is slower anyway, which the device said and the reasoning did not:
    // `decode_max_ms` went from 78 to 91, and sweeping the inner loop from a
    // division to a multiply changed nothing (90). The cost is the lookups, not
    // the copying -- the two-pass version does 43200 palette reads a frame and
    // then moves bytes, while the one-pass version does 76800 palette reads,
    // one per *panel* pixel, because every output pixel now needs its own
    // lookup. Moving 153600 bytes is cheaper than 33600 extra table reads on
    // this chip.
    {
        int64_t t=esp_timer_get_time();
        av_expand_indexed_wire(buf,AV_STRIPE_PIXELS,s.palette);
        atomic_fetch_add(&s.expand_us,(uint32_t)(esp_timer_get_time()-t));
    }
    {
        int64_t t=esp_timer_get_time();
        enlarge_stripe(buf);
        atomic_fetch_add(&s.enlarge_us,(uint32_t)(esp_timer_get_time()-t));
    }
    }
    // Painted into the stripe on its way to the panel: the buffer already
    // exists, so the overlay costs no memory, and a stripe it does not touch
    // reaches the panel untouched.
    {
        int64_t t=esp_timer_get_time();
        overlay_stripe(y,buf);
        atomic_fetch_add(&s.overlay_us,(uint32_t)(esp_timer_get_time()-t));
    }
    // After the overlay, so what is kept is what the panel is about to be
    // given rather than what it would have been had the text and its box not
    // been painted into the stripe on the way past.
    if(s_shot_taking) {
        if(y==(unsigned)AV_VIDEO_Y_OFFSET) s_shot_rows=0;
        if(s_shot_rows<SHOT_ROWS) shot_take(y,buf);
    }
    {
        // Queue it, and let the bus start on it now rather than after the next
        // stripe has been built.
        //
        // This used to submit and then wait, which made the panel strictly
        // serial with the CPU: build a stripe, send it, wait for it to finish,
        // build the next. Measured on the device with the link stopped out of
        // the way, that costs 45.0 ms a frame; queueing ahead of one stripe
        // costs 32.0 ms, and the bus's own floor is 30.7. The thirteen
        // milliseconds were the processor standing still beside a DMA engine,
        // and the reason an earlier attempt to fix it appeared to fail is that
        // it was measured during a session the link was pacing -- so the frame
        // rate could not move however much time came back.
        int64_t tt=esp_timer_get_time();
        esp_err_t e=bsp_display_raw_submit_nowait(y,AV_STRIPE_ROWS,buf);
        if(e==ESP_OK) s.stripe_live++;
        atomic_fetch_add(&s.submit_us,(uint32_t)(esp_timer_get_time()-tt));
        if(e!=ESP_OK) return false;
    }
    if(s.stripe_live>=2) {
        // Drained down to one outstanding transfer before the next stripe is
        // built. One deep is the whole point: at two, the buffer the CPU is
        // about to write would still be on the wire.
        int64_t wait_start=esp_timer_get_time();
        if(bsp_display_raw_drain(1,200)!=ESP_OK) return false;
        s.stripe_live--;
        atomic_fetch_add(&s.panel_wait_us,(uint32_t)(esp_timer_get_time()-wait_start));
    }
    return true;
}
static void drain_display(void) {
    // Up to two can be outstanding now -- one queued ahead of the stripe being
    // built -- and releasing the panel with a transfer still in flight would
    // hand the displays back mid-write. Drained to zero, not to "the last wait
    // returned", which with a queue is no longer the same thing.
    for(unsigned i=0;i<10;i++) {
        esp_err_t e=bsp_display_raw_drain(s.stripe_live>0?s.stripe_live:1,200);
        if(e==ESP_OK) { s.stripe_live=0; return; }
        fail("LCD DMA drain failed; buffers retained");
    }
    // Fault-only recovery, never free live DMA memory or force-delete a worker.
    ESP_LOGE(TAG,"LCD DMA unresponsive for 2s; fault reboot without freeing buffers");
    esp_restart();
}
// The SMPTE-style bars, in the order they are conventionally drawn: white,
// yellow, cyan, green, magenta, red, blue. Seven equal vertical bars fill the
// width exactly because 320/7 is not a whole number of pixels -- each bar takes
// the pixels its share covers, so the last one absorbs the remainder rather than
// leaving a strip of untouched memory at the right edge.
static void draw_color_bars(ui_surface_t surface) {
    static const uint16_t bars[7] = {
        0xFFFFu, // white
        0xFFE0u, // yellow
        0x07FFu, // cyan
        0x07E0u, // green
        0xF81Fu, // magenta
        0xF800u, // red
        0x001Fu, // blue
    };
    // Filled from the surface's own origin: ui_text_fill clips to the rows this
    // stripe covers, so a y of 0 would draw nothing on every stripe but the
    // first.
    for (unsigned i=0;i<7;i++) {
        int left=(int)((unsigned)AV_WIDTH*i/7u);
        int right=(int)((unsigned)AV_WIDTH*(i+1u)/7u);
        ui_text_fill(surface,left,surface.origin_y,right-left,surface.rows,bars[i]);
    }
}
// Whether the waiting screen needs painting again.
//
// Asked from the video task main loop rather than decided once when the session
// begins. The link state is reset to "dialling" at the start of every session,
// so a decision taken at that instant can only ever see "dialling" -- it paints
// "connecting" and never sees the failure that follows, and the screen then sits
// on "connecting" for as long as the device keeps retrying. Asking repeatedly
// makes the screen follow the state instead of sampling it once at the worst
// possible moment.
static bool waiting_screen_stale(void)
{
    // Nothing owns the panel until the first frame arrives. After that the
    // frames do, and repainting the bars underneath them would flash.
    if (atomic_load(&s.media_started)) return false;
    if (!waiting_drawn) return true;
    if (strcmp(s.channel,shown_channel)) return true;
    if ((int)s.menu.view!=waiting_view) return true;
    if ((int)atomic_load(&s.link)!=waiting_link) return true;
    // While an overlay is up it is repainted on a slow timer: its readings
    // change and a key press has to show. The bars beneath it do not, which is
    // why this is not simply every pass.
    if (s.menu.view!=UI_VIEW_VIDEO &&
        esp_timer_get_time()-waiting_painted_us>=(int64_t)WAITING_REFRESH_MS*1000) return true;
    return false;
}

// Paint the colour-bar waiting screen, with whatever overlay is up.
//
// Returns false when the panel stopped accepting data, which the caller treats
// as a display fault.
//
// The overlay is painted here too, and that is what makes the status page
// reachable at all. That page is drawn by overlay_stripe(), whose only other
// caller is the JPEG output path -- so an overlay could otherwise appear only
// over a moving picture. A device with no picture would then have no status
// page, and the status page is the only way to the setup screen: the state that
// needs setup most was the one state that could not reach it.
static bool paint_waiting_screen(void)
{
    ui_menu_overlay_begin(&menu_overlay,&s.menu);
    waiting_drawn=true;
    waiting_view=(int)s.menu.view;
    waiting_link=(int)atomic_load(&s.link);
    waiting_painted_us=esp_timer_get_time();
    snprintf(shown_channel,sizeof(shown_channel),"%s",s.channel);
for(unsigned y=0;y<AV_HEIGHT && !stopping();y+=AV_STRIPE_ROWS) {
    // The waiting screen runs when no video task is drawing, so it uses the
    // first buffer and does not take turns with anyone.
    ui_surface_t strip=stripe_surface(y,s.stripe[0]);
    draw_color_bars(strip);
    // One panel holding both lines, drawn as a unit: the name, and below it
    // the reason there is no picture when there is one worth giving.
    //
    // Laid out together rather than positioned one at a time, so the panel
    // is centred on what it holds and both lines share a left edge. Sized to
    // the wider line, and kept a margin from the screen edge so it reads as
    // a card over the pattern rather than a band across it.
    //
    // The whole panel is drawn by every strip its box touches, not just the
    // strip the first line starts in. Overlap is the test, and it is taken
    // against the box each line occupies rather than its glyphs alone: a
    // line begins above its glyphs (UI_TEXT_PAD_Y) and ends below them, so
    // measuring glyphs leaves the strip holding the padding to skip the
    // line, and testing where a line *starts* cuts it at the boundary. Lines
    // straddle strips by construction -- strips are 16 rows and a line is 24
    // -- so either mistake shows as text sliced through the middle.

    // What the viewer is waiting for, and why.
    //
    // The connection has three states and the screen shows a different
    // thing for each. The one that matters here is the middle: while the
    // dial is in progress nothing has been concluded, and saying "cannot
    // reach the server" during those seconds is not a premature verdict so
    // much as a false one -- it sends the viewer to change a setting that
    // was never the problem, and the connection usually succeeds a moment
    // later. Failure is reported only once the attempt is over.
    const link_state_t link=(link_state_t)atomic_load(&s.link);
    const bool dialling = link==LINK_TRYING;
    const bool failed   = link==LINK_DOWN && !atomic_load(&s.media_started);

    // Keyed on whether the server answered rather than on whether an address is
    // stored, because a stored address that is wrong -- a stale lease, a
    // mistyped port -- leaves the device just as stuck and is the case most
    // worth naming.
    //
    // There is deliberately no "no address set" wording. The setup page requires
    // that field, so a device which got through setup always has one; a device
    // that never went through it is in setup rather than here. The line would
    // name a state that cannot be reached, and a message that cannot be true is
    // worse than none: it sends the viewer to look for a setting they already
    // have.
    char heading[UI_MENU_NAME_MAX+16];
    const char *reason=NULL;
    if (failed) {
        reason="Cannot reach server";
    }
    bool has_heading=s.channel[0]!=0;
    if (has_heading) {
        current_channel_name(heading,sizeof(heading));
    } else {
        // No channel named yet, so the line carries the state instead, which
        // is worth saying only because the alternative is an empty panel.
        //
        // Both remaining cases are covered: still dialling, and connected
        // but not yet sent a channel. They read differently because they are
        // different -- the first is waiting on the network, the second on the
        // server's transcoder, which takes seconds on a live channel -- and a
        // viewer who sees the same words for both cannot tell whether the
        // wait is normal.
        snprintf(heading,sizeof(heading),
                 dialling ? "Connecting" : "Opening channel");
        has_heading=true;
    }

    // How to get out of this state, shown whenever the device cannot reach
    // its server.
    //
    // Without it the screen is a dead end: the way to the setup page is a
    // long press on an arrow *on the status page*, and the status page is
    // opened by double-clicking that same arrow. Nothing about the device
    // suggests either gesture, so a viewer who does not already know them
    // has no move to make -- and this is the screen where they need one. The
    // other half of the fix is that the status page now draws over this
    // screen; telling the viewer about a screen that cannot appear would be
    // worse than saying nothing.
    const char *hint=NULL;
    if (reason) {
        hint="Hold OK to exit";
    }

    // Measured with the padding each line is drawn with, not the glyphs
    // alone: ui_text_draw paints its own box around the text, so a panel
    // sized to the glyph run alone is a few pixels narrower than the text
    // inside it and the two edges disagree.
    //
    // Bounded so a long channel name shrinks the panel rather than running
    // off the screen; the lines themselves are clipped by the surface.
    const int PANEL_MAX=(int)AV_WIDTH-24;
    int text_w=0;
    if (has_heading) text_w=ui_text_width(heading);
    if (reason) {
        int rw=ui_text_width(reason);
        if (rw>text_w) text_w=rw;
    }
    if (hint) {
        int hw=ui_text_width(hint);
        if (hw>text_w) text_w=hw;
    }
    // The lines are stacked; the count is what the height follows.
    int lines=0;
    if (has_heading) lines++;
    if (reason) lines++;
    if (hint) lines++;
    if (lines==0) lines=1;

    int panel_w=text_w+2*UI_TEXT_PAD_X;
    if (panel_w>PANEL_MAX) panel_w=PANEL_MAX;
    int panel_x=((int)AV_WIDTH-panel_w)/2;
    const int line_step=(int)UI_TEXT_LINE_H+2*(int)UI_TEXT_PAD_Y;
    int panel_h=lines*line_step;
    int panel_y=(int)AV_HEIGHT/2-panel_h/2;

    if ((int)y<panel_y+panel_h && panel_y<(int)(y+AV_STRIPE_ROWS)) {
        // The panel behind the lines. One rectangle, not one per line, so
        // they read as a single object with a shared background, and every
        // line starts at the same left edge -- centring each on its own made
        // them disagree whenever their widths did.
        ui_text_fill(strip,panel_x,panel_y,panel_w,panel_h,UI_COLOR_BOX);
        int row=0;
        if (has_heading) {
            ui_text_draw(strip,panel_x+UI_TEXT_PAD_X,panel_y+row*line_step+UI_TEXT_PAD_Y,
                         heading,UI_COLOR_TEXT,UI_COLOR_BOX);
            row++;
        }
        if (reason) {
            // Dimmer than a name, because it explains rather than
            // identifies: the name is what the viewer is looking for, this
            // is why it has not appeared.
            ui_text_draw(strip,panel_x+UI_TEXT_PAD_X,panel_y+row*line_step+UI_TEXT_PAD_Y,
                         reason,UI_COLOR_DIM,UI_COLOR_BOX);
            row++;
        }
        if (hint) {
            ui_text_draw(strip,panel_x+UI_TEXT_PAD_X,panel_y+row*line_step+UI_TEXT_PAD_Y,
                         hint,UI_COLOR_DIM,UI_COLOR_BOX);
        }
    }
    // On top of the panel, so an overlay covers it rather than competing
    // with it: the status page and the channel list paint their own
    // backgrounds across the full width for exactly this reason.
    overlay_stripe(y,s.stripe[0]);
    // Through the queueing entry point and drained in the same breath: the
    // waiting screen redraws on its own schedule and wants the simple
    // synchronous behaviour, not the overlap playback uses.
    if(bsp_display_raw_submit_nowait(y,AV_STRIPE_ROWS,s.stripe[0])!=ESP_OK ||
       bsp_display_raw_drain(1,200)!=ESP_OK) {  // one in flight at a time here
        fail("waiting screen DMA"); break;
    }
}
    return true;
}

// Between sessions the video worker has released raw mode. Borrow it only for
// a stale waiting screen, and return it before HTTPS or worker creation.
static bool paint_connection_waiting_screen(void) {
    if(!waiting_screen_stale())return true;
    if(bsp_display_raw_claim()!=ESP_OK) {
        fail("connection display claim");
        return false;
    }
    s.stripe[0]=s_lvgl_stripes;
    bool painted=paint_waiting_screen();
    drain_display(); // also drains a stripe left outstanding by a failed paint
    if(bsp_display_raw_release()!=ESP_OK) {
        fail("connection display release");
        esp_restart(); // cannot hand the panel back with its raw callback installed
        return false;
    }
    return painted && !stopping();
}

static void video_housekeeping(int64_t *last) {
    int64_t began=esp_timer_get_time();
    if(began-*last<500000)return;
    *last=began;
    // A frame can be ready continuously. A plain taskYIELD only serves peers
    // and otherwise starves the session/UI owner and IDLE indefinitely. At a
    // completed, unlocked frame boundary, run pending background work once,
    // then restore media priority immediately. There is no sleep or FPS cap.
    UBaseType_t priority=uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL,tskIDLE_PRIORITY);
    taskYIELD();
    vTaskPrioritySet(NULL,priority);
    atomic_fetch_add(&s.housekeeping_us,(uint32_t)(esp_timer_get_time()-began));
}

static void video_task(void *arg) {
    (void)arg;
    int64_t pkt_start_us=0;
    int64_t last_frame_submit_us=0;
    int64_t progress_at=esp_timer_get_time();
    int64_t housekeeping_at=progress_at;
    uint32_t progress_previous[14]={0};
    bool pkt_ok=true;
    bool claimed=bsp_display_raw_claim()==ESP_OK;
    if(!claimed) { fail("raw display claim"); goto done; }
    // Painted before the first frame so the panel never shows uninitialised
    // memory, and repainted from the main loop below whenever what it shows
    // changes -- the connection state moves on its own, and the screen has to
    // move with it.
    if (!paint_waiting_screen()) goto done;
    while(!stopping()) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        atomic_store(&s.video_cpu_us,own_cpu_us());
#endif
        // The connection state moves without a frame ever arriving: the dial
        // gives up after a few seconds and the screen has to say so. This is the
        // only place its owner runs while it waits, so this is where the check
        // belongs.
        if (waiting_screen_stale() && !paint_waiting_screen()) break;
        if (!refresh_info_bars()) break;
        av_complete_frame_t frame;
        if(!xQueueReceive(s.video,&frame,pdMS_TO_TICKS(20))) { if(rx_done()) break; continue; }
        if(!av_frame_complete(&frame)) { fail("incomplete display frame"); break; }
        ui_menu_overlay_begin(&menu_overlay,&s.menu);
        int64_t frame_draw_started=esp_timer_get_time();
        for(unsigned stripe_index=0;stripe_index<frame.count;stripe_index++) {
        video_t v=frame.stripes[stripe_index];
        // All stripe data is already resident. The display never waits for
        // network input mid-frame, only for the audio clock's initial start.
        if(v.frame_start) {
            while(!stopping() && !(xEventGroupGetBits(s.events)&CLOCK_READY)) {
                if(xEventGroupGetBits(s.events)&AUDIO_DONE) { fail("no audio clock"); break; }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            s.skipping=stopping() || estimated_pts()-(int64_t)v.pts>100;
            s.drawn_stripes=0;
            if(s.skipping) { s.dropped++; atomic_fetch_add(&s.rx_frame_skipped,1u); }
        }
        if(!s.skipping) {
            if(v.packet_start) { pkt_start_us=esp_timer_get_time(); pkt_ok=true; }
            if(!v.dropped) {
#if AV_VIDEO_Y_OFFSET > 0
                if(s.decoded == 0 && v.stripe == 0) {
                    memset(s.stripe[0], 0, STRIPE_BYTES);
                    for (unsigned cy = 0; cy < (unsigned)AV_VIDEO_Y_OFFSET; ) {
                        unsigned rows = (unsigned)AV_VIDEO_Y_OFFSET - cy;
                        if (rows > AV_STRIPE_ROWS) rows = AV_STRIPE_ROWS;
                        bsp_display_raw_submit_nowait(cy, rows, s.stripe[0]);
                        bsp_display_raw_drain(1, 200);
                        cy += rows;
                    }
                    const unsigned bottom_start = AV_HEIGHT - (unsigned)AV_VIDEO_Y_OFFSET;
                    for (unsigned cy = bottom_start; cy < AV_HEIGHT; ) {
                        unsigned rows = AV_HEIGHT - cy;
                        if (rows > AV_STRIPE_ROWS) rows = AV_STRIPE_ROWS;
                        bsp_display_raw_submit_nowait(cy, rows, s.stripe[0]);
                        bsp_display_raw_drain(1, 200);
                        cy += rows;
                    }
                }
#endif
                unsigned y=(unsigned)AV_VIDEO_Y_OFFSET + (unsigned)v.stripe*AV_STRIPE_ROWS;
                if(!push_stripe(&v,y)) pkt_ok=false;
                s.drawn_stripes++;
            }
        }
        if(!s.skipping && v.packet_end) {
            int64_t start=pkt_start_us;
            bool drawn=pkt_ok;
            if (s.drawn_stripes > atomic_load(&s.rx_max_stripes)) {
                atomic_store(&s.rx_max_stripes, s.drawn_stripes);
            }
            if(drawn && s.drawn_stripes>=AV_STRIPES) {
                s.decoded++;
                // Completion of frame submission, not LCD DMA completion.
                int64_t submitted=esp_timer_get_time();
                if(last_frame_submit_us)frame_max(&s.frame_submit_gap_max_us,(uint32_t)(submitted-last_frame_submit_us));
                last_frame_submit_us=submitted;
                if(s.menu.view==UI_VIEW_MENU)atomic_fetch_add(&s.menu_frames,1u);
                s.drawn_stripes=0;
                // A capture is taken from a whole frame, and only once the
                // session has settled: the first seconds after a channel change
                // are a fade-in or a caption, which would say nothing about how
                // the device renders an ordinary picture.
                //
                // The buffer is both taken and given back inside the frame that
                // is kept. Allocating it when the capture was armed -- before
                // the first CONFIG arrives -- starved the channel list's parse,
                // and allocating it one frame ahead printed from a null pointer
                // and faulted. Filling it on the way past and handing it back
                // here costs the frame one allocation and holds nothing across
                // the periods when the heap is wanted for something else.
                if(s_shot_wanted && !s_shot_taking
                   && ++s_shot_frames >= SHOT_AFTER_FRAMES) {
                    s_shot=heap_caps_malloc(SHOT_BYTES,MALLOC_CAP_8BIT);
                    if(s_shot) { s_shot_rows=0; s_shot_taking=true; }
                    else {
                        ESP_LOGW(TAG,"No memory for a screen capture; not taken");
                        s_shot_wanted=false;
                    }
                } else if(s_shot_taking) {
                    shot_print();
                    s_shot_taking=false;
                }
            }
            // Microseconds, and this line used to be wrong in a way that made a
            // broken instrument look like a fast device. `elapsed` was already
            // milliseconds -- the divisor below was applied when it was taken --
            // and it was then accumulated into a field named `panel_us` and
            // divided by a thousand once more on the way out. Measured, that
            // reported 6 ms of drawing for 107 frames, which is 0.06 ms a frame,
            // while `inflate_ms` in the same line reported 12.3 ms a frame for
            // work that happens inside this very interval. Two numbers in one
            // line that cannot both be true is what a unit bug looks like.
            uint32_t elapsed_us=(uint32_t)(esp_timer_get_time()-start);
            unsigned elapsed_ms=elapsed_us/1000;
            if(elapsed_ms>s.decode_max_ms) s.decode_max_ms=elapsed_ms;
            atomic_fetch_add(&s.panel_us,elapsed_us);
            atomic_fetch_add(&s.panel_frames,1u);
            if(!drawn && !stopping()) fail("indexed stripe decode/geometry/DMA error");
        }
        if(v.data) ring_release(&v);
        }
        frame_max(&s.frame_draw_max_us,(uint32_t)(esp_timer_get_time()-frame_draw_started));
        video_housekeeping(&housekeeping_at);
        // Observe from the worker itself: a continuously runnable video worker
        // can starve the lower-priority session logger. Do not change priority,
        // pacing, frame selection or queue policy just to make metrics appear.
        int64_t progress_now=esp_timer_get_time();
        if(progress_now-progress_at>=10000000) {
            uint32_t values[14]={
                atomic_load(&s.decoded),atomic_load(&s.dropped),
                atomic_load(&s.inflate_us),atomic_load(&s.expand_us),
                atomic_load(&s.overlay_us),atomic_load(&s.panel_wait_us),0,0,0,0,
                atomic_load(&s.submit_us),0,0,atomic_load(&s.housekeeping_us)};
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            values[6]=own_cpu_us();values[7]=atomic_load(&s.rx_cpu_us);
            values[8]=atomic_load(&s.audio_cpu_us);values[9]=(uint32_t)ulTaskGetIdleRunTimeCounter();
            values[11]=named_cpu_us("wifi");values[12]=named_cpu_us("tiT");
#endif
            uint32_t delta[14];
            for(unsigned i=0;i<14;i++)delta[i]=values[i]-progress_previous[i];
            ESP_LOGI(TAG,"VIDEO_PROGRESS interval_ms=%"PRIu32" frames=%"PRIu32" skipped=%"PRIu32
                " inflate_ms=%"PRIu32" expand_ms=%"PRIu32" overlay_ms=%"PRIu32" dma_wait_ms=%"PRIu32
                " cpu_ms=%"PRIu32"/%"PRIu32"/%"PRIu32" idle_ms=%"PRIu32" submit_ms=%"PRIu32
                " wifi_cpu_ms=%"PRIu32" tcpip_cpu_ms=%"PRIu32
                " heap_fail=%"PRIu32" heap=%u largest=%u cooperate_ms=%"PRIu32
                " silence_samples=%"PRIu32" audio_samples=%"PRIu32,
                (uint32_t)((progress_now-progress_at)/1000),delta[0],delta[1],
                delta[2]/1000,delta[3]/1000,delta[4]/1000,delta[5]/1000,
                delta[6]/1000,delta[7]/1000,delta[8]/1000,delta[9]/1000,delta[10]/1000,
                delta[11]/1000,delta[12]/1000,
                (uint32_t)atomic_load(&heap_media_failures),(unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),delta[13]/1000,
                (uint32_t)atomic_load(&s.silence_samples),(uint32_t)atomic_load(&s.submitted_samples));
            memcpy(progress_previous,values,sizeof(values));progress_at=progress_now;
        }
    }
    // Fault recovery preserves ownership until completion or a hardware reboot.
    drain_display();
    if(bsp_display_raw_release()!=ESP_OK) fail("raw release");
done:
    xEventGroupSetBits(s.events,VIDEO_DONE);
    vTaskDelete(NULL);
}
// Exercise the actual ROM inflater at region boundaries once per boot before
// any receiver/audio/display worker exists. Uses only already-borrowed storage.
static bool verify_frame_decoder(void) {
    static bool verified;
    if(verified)return true;
    // zlib(level=1), raw[i]=(i*37+(i>>5))&255, 3840 indexed pixels.
    static const uint8_t compressed[]={
        0x78,0x01,0x85,0xd7,0x05,0x5b,0x54,0x41,0x14,0x06,0x60,0x4c,0xb0,0xb1,0xbb,0x17,
        0x1b,0xbb,0x7b,0x31,0xc1,0xc6,0xee,0x06,0x1b,0xbb,0x7b,0xb1,0xb1,0xbb,0x17,0x13,
        0x6c,0xec,0x6e,0xb0,0xb1,0xbb,0x17,0x1b,0xbb,0x5b,0x6f,0x4e,0x9e,0x99,0xfb,0x0f,
        0xde,0x67,0xe2,0x9c,0xef,0x73,0xf3,0xf2,0x0b,0x9a,0xb7,0xef,0x41,0xbc,0x7c,0xf5,
        0xfa,0x2f,0x3c,0xf8,0x38,0x61,0xc1,0x86,0x83,0x96,0x1c,0x79,0x92,0xa8,0x70,0xe3,
        0xa1,0xcb,0x8f,0x3f,0x4f,0x5a,0xac,0xd9,0x88,0xd0,0xa8,0x58,0xcf,0x52,0xad,0xc6,
        0xac,0x39,0xf3,0x36,0x75,0xd9,0xb6,0xe3,0xd7,0x9f,0xff,0x90,0xae,0x42,0x87,0x09,
        0x1b,0x2e,0x7e,0xce,0x58,0xb9,0xf3,0xe4,0xcd,0x57,0xbe,0x65,0xf1,0x09,0x0c,0x89,
        0xb8,0xf1,0x2b,0x47,0x8d,0x1e,0x33,0x77,0xde,0xfe,0x9b,0xbb,0x76,0xef,0x39,0x7b,
        0xee,0xc5,0xc9,0x53,0xa7,0xef,0xfc,0xfd,0x0f,0xe3,0xe7,0xaf,0x3f,0x60,0xd1,0xa1,
        0x18,0x0f,0x6f,0xff,0x21,0xcb,0x8e,0x3d,0x4b,0x52,0xb4,0xe9,0xf0,0x95,0x27,0x5f,
        0x26,0x2f,0xd1,0x62,0xd4,0xaa,0x53,0xaf,0x53,0x96,0x6e,0x3d,0x76,0xed,0xd9,0x77,
        0x69,0xca,0xb5,0x0b,0x0e,0x8f,0xfe,0x94,0xa1,0x52,0xa7,0x49,0x9b,0x2e,0x7f,0xcd,
        0x5c,0xb5,0xeb,0xd4,0xad,0xd7,0x7e,0x64,0xab,0xd6,0x6d,0xfa,0xf6,0x9b,0xbf,0x73,
        0xd6,0xec,0x39,0x6b,0xd7,0x9d,0x7f,0x16,0x3c,0xa7,0x05,0xcf,0x6e,0xc1,0x73,0x59,
        0xf0,0x1c,0x16,0x3c,0x9b,0xfc,0xf4,0x9c,0x91,0xf2,0xd3,0xb3,0x07,0xc8,0x4f,0xcf,
        0xe5,0x2e,0x3f,0x3d,0x47,0x98,0xfc,0xf4,0x6c,0xbe,0xd2,0xcb,0x75,0x46,0xbe,0x92,
        0x5e,0xae,0x3d,0x60,0x9a,0xf4,0x72,0x5d,0xee,0x85,0xa4,0x97,0xeb,0x08,0xbb,0x20,
        0xbd,0x5c,0x9b,0x6f,0x1f,0xd9,0xdb,0x53,0x78,0x29,0x64,0x6f,0x4f,0xe1,0x6d,0x93,
        0xbd,0x3d,0x85,0xd7,0x48,0xf6,0xf6,0x14,0xde,0x47,0xd9,0xdb,0x53,0x78,0x73,0x25,
        0x5f,0x43,0xe5,0x95,0x94,0x7c,0x0d,0x95,0x77,0x5d,0xf2,0x35,0x54,0xde,0x60,0xc9,
        0xd7,0x50,0x79,0xe9,0x25,0x5f,0x43,0xe5,0xed,0x15,0xff,0x5c,0x8d,0xd7,0x52,0xfc,
        0x73,0x35,0xde,0x4f,0xf1,0xcf,0xd5,0x78,0x4b,0xc5,0x3f,0x57,0xe3,0x55,0x14,0xff,
        0x5c,0x8d,0x77,0x5f,0x38,0x58,0x74,0xde,0x68,0xe1,0x60,0xd1,0x79,0xd9,0x85,0x83,
        0x45,0xe7,0x1d,0x15,0x0e,0x16,0x9d,0xd7,0x51,0x38,0x58,0x74,0x5e,0x5c,0xd1,0xdc,
        0x33,0x78,0xab,0x45,0x73,0xcf,0xe0,0x55,0x17,0xcd,0x3d,0x83,0xf7,0x54,0x34,0xf7,
        0x0c,0xde,0x44,0xd1,0xdc,0x33,0x78,0x79,0x05,0x63,0xd9,0xe4,0x9d,0x16,0x8c,0x65,
        0x93,0xd7,0x5d,0x30,0x96,0x4d,0x5e,0x62,0xc1,0x58,0x36,0x79,0x1b,0x05,0x63,0xd9,
        0xe4,0xd5,0x85,0xb7,0x06,0xe2,0xbd,0x81,0xb7,0x06,0xe2,0xcd,0x80,0xb7,0x06,0xe2,
        0x15,0x81,0xb7,0x06,0xe2,0x5d,0x82,0xb7,0x06,0xe2,0xf5,0x03,0x97,0x1a,0xe6,0xa5,
        0x02,0x97,0x1a,0xe6,0xed,0x00,0x97,0x1a,0xe6,0x35,0x01,0x97,0x1a,0xe6,0x7d,0x01,
        0x97,0x1a,0xe6,0x2d,0x80,0x76,0x2e,0xc1,0x2b,0x03,0xed,0x5c,0x82,0x77,0x0b,0xda,
        0xb9,0x04,0x6f,0x18,0xb4,0x73,0x09,0x5e,0x26,0x68,0xe7,0x12,0xbc,0x03,0x40,0x24,
        0x20,0x79,0x6d,0x80,0x48,0x40,0xf2,0xfe,0x00,0x91,0x80,0xe4,0xad,0x00,0x22,0x01,
        0xc9,0xab,0x02,0x44,0x02,0x92,0xf7,0x88,0x4f,0x2c,0x14,0x6f,0x1c,0x9f,0x58,0x28,
        0x5e,0x2e,0x3e,0xb1,0x50,0xbc,0x13,0x7c,0x62,0xa1,0x78,0x5d,0xf8,0xc4,0x42,0xf1,
        0x12,0x70,0x81,0x8a,0xe6,0xad,0xe3,0x02,0x15,0xcd,0xab,0xc5,0x05,0x2a,0x9a,0xf7,
        0x82,0x0b,0x54,0x34,0x6f,0x0a,0x17,0xa8,0x68,0x5e,0x01,0x36,0xef,0x31,0xbc,0x73,
        0x6c,0xde,0x63,0x78,0xbd,0xd8,0xbc,0xc7,0xf0,0x92,0xb1,0x79,0x8f,0xe1,0x6d,0x61,
        0xf3,0x1e,0xc3,0x6b,0xc0,0xc4,0x51,0x96,0xf7,0x9e,0x89,0xa3,0x2c,0x6f,0x36,0x13,
        0x47,0x59,0x5e,0x71,0x26,0x8e,0xb2,0xbc,0xab,0x4c,0x1c,0x65,0x79,0x03,0xe9,0xb4,
        0xcc,0xf1,0xd2,0xd2,0x69,0x99,0xe3,0xed,0xa6,0xd3,0x32,0xc7,0x6b,0x4e,0xa7,0x65,
        0x8e,0xf7,0x9d,0x4e,0xcb,0x1c,0x6f,0x31,0x15,0xe6,0x79,0x5e,0x79,0x2a,0xcc,0xf3,
        0xbc,0xbb,0x54,0x98,0xe7,0x79,0x23,0xa9,0x30,0xcf,0xf3,0xb2,0x52,0x61,0x9e,0xe7,
        0x1d,0x26,0xbb,0x06,0xc0,0x6b,0x4f,0x76,0x0d,0x80,0xe7,0x46,0x76,0x0d,0x80,0x17,
        0x4a,0x76,0x0d,0x80,0xe7,0x43,0x76,0x0d,0x80,0x17,0x43,0x54,0x21,0x88,0x17,0x4c,
        0x54,0x21,0x88,0xe7,0x45,0x54,0x21,0x88,0x17,0x45,0x54,0x21,0x88,0x17,0x48,0x54,
        0x21,0x88,0xe7,0x81,0x9b,0x1a,0xc8,0x0b,0xc7,0x4d,0x0d,0xe4,0xf9,0xe1,0xa6,0x06,
        0xf2,0x62,0x71,0x53,0x03,0x79,0x21,0xb8,0xa9,0x81,0x3c,0x6f,0x54,0x24,0x61,0x5e,
        0x34,0x2a,0x92,0x30,0x2f,0x08,0x15,0x49,0x98,0xe7,0x89,0x8a,0x24,0xcc,0x8b,0x40,
        0x45,0x12,0xe6,0xf9,0xff,0x07,0xf9,0xbd,0x7d,0x6a,
    };
    const unsigned cuts[]={1,2,3,7,sizeof(compressed)-4,sizeof(compressed)-1};
    uint32_t boundary=0;
    for(unsigned region=0;region<AV_FRAME_REGIONS;region++) {
        boundary+=s_frame_regions[region].size;
        for(unsigned cut=0;cut<sizeof(cuts)/sizeof(cuts[0]);cut++)for(unsigned corrupt=0;corrupt<2;corrupt++) {
            if(!av_frame_ring_init(&s.frame_ring,s_frame_regions))return false;
            s.frame_ring.head=boundary-cuts[cut];atomic_store(&s.frame_ring.tail,s.frame_ring.head);
            video_t stripe={.length=sizeof(compressed)};
            if(!av_frame_ring_alloc(&s.frame_ring,sizeof(compressed),&stripe))return false;
            uint32_t cursor=stripe.data_start,left=sizeof(compressed),offset=0;
            while(left) {
                uint8_t *part=NULL;uint32_t n=av_frame_ring_span(&s.frame_ring,&cursor,left,&part);
                if(!n)return false;
                memcpy(part,compressed+offset,n);
                if(corrupt && n==left)part[n-1]^=1u;
                offset+=n;left-=n;
            }
            size_t used=0,made=0;
            tinfl_status status=inflate_frame_stripe(&stripe,s.stripe[0],&used,&made);
            if(corrupt) { if(status==TINFL_STATUS_DONE)return false;continue; }
            if(status!=TINFL_STATUS_DONE || used!=sizeof(compressed) || made!=AV_STRIPE_PIXELS)return false;
            for(unsigned i=0;i<AV_STRIPE_PIXELS;i++)
                if(s.stripe[0][i]!=(uint8_t)(i*37u+(i>>5)))return false;
        }
    }
    verified=av_frame_ring_init(&s.frame_ring,s_frame_regions);
    if(verified)ESP_LOGI(TAG,"Frame span decoder self-test passed (%u boundary/checksum cases)",AV_FRAME_REGIONS*12u);
    return verified;
}
static QueueHandle_t create_video_queue(void) {
    // This queue holds descriptors, not pixels or DMA data. Use the RTC
    // remainder beside the session state before consuming Wi-Fi's SRAM.
    const size_t bytes=sizeof(StaticQueue_t)+AV_VIDEO_ITEMS*sizeof(av_complete_frame_t);
    QueueHandle_t queue=NULL;
    bool rtc=heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM)>=bytes+32u;
    if(rtc)queue=xQueueCreateWithCaps(AV_VIDEO_ITEMS,sizeof(av_complete_frame_t),
                                    MALLOC_CAP_RTCRAM|MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!queue) {
        rtc=false;
        queue=xQueueCreateWithCaps(AV_VIDEO_ITEMS,sizeof(av_complete_frame_t),
                                  MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
    ESP_LOGI(TAG,"VIDEO_QUEUE_WORKSPACE rtc=%u bytes=%u created=%u",
             (unsigned)rtc,(unsigned)bytes,(unsigned)(queue!=NULL));
    return queue;
}
static bool allocate_session(void) {
    s.audio=xQueueCreate(PCM_QUEUE,sizeof(audio_t));
    // Deep enough to hold one finished frame waiting while the next is being
    // gathered, and no deeper: the whole point of this queue is to let old
    // pictures go rather than draw them late.
    // Deep enough to hold one finished frame waiting while the next is being
    // gathered, and no deeper: the whole point of this queue is to let old
    // pictures go rather than draw them late.
    //
    // Three was tried, to give the pipeline a slot for the frame that is being
    // written to the panel, and reverted with the packet buffers: the memory
    // that mattered was the channel list's, and every slot added anywhere in
    // the session is paid for out of the same heap before the first CONFIG is
    // parsed.
    s.video=create_video_queue();
    // CONFIG is parsed and copied before any video can enter the ring. The
    // disconnected social receiver lends this same workspace for both phases.
    bool frame_buffers_ok=av_frame_ring_init(&s.frame_ring,s_frame_regions);
    // Internal memory, not DMA-capable: the panel reads it through the same
    // DMA submission as before, but the buffer also has to hold the index bytes
    // while they are expanded in place, so it is written by the CPU first.
    // Two, so the bus can be working on one stripe while the next is built.
    // The extra 10 KB is the whole of what the overlap costs, and it is the
    // trade the earlier attempt got backwards: it was recorded as costing
    // memory and buying nothing, when the measurement that said so was taken
    // during a session the link was pacing. Measured with the link stopped
    // out of the way, the overlap is worth 45.0 ms a frame down to 32.0.
    s.stripe[0]=s_lvgl_stripes;
    s.stripe[1]=s_lvgl_stripes+STRIPE_BYTES;
    s.stripe_next=0;
    s.stripe_live=0;
    // About 11 KB. Heap rather than a static: this struct is static, and a
    // session that never starts should not be paying for it.
    s.inflate=heap_caps_malloc(sizeof(tinfl_decompressor),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    s.palette_ready=false;
    s.drawn_stripes=0;
    s.skipping=false;
    if(!s.audio || !s.video || !frame_buffers_ok || !s.stripe[0] || !s.stripe[1] || !s.inflate) return false;
    if(!verify_frame_decoder()) { ESP_LOGE(TAG,"Frame span decoder self-test failed");return false; }
    return true;
}
static void free_session(void) {
    if(s.audio) vQueueDelete(s.audio);
    if(s.video) vQueueDeleteWithCaps(s.video);
    s.audio=s.video=NULL;
    // Borrowed regions remain caller-owned; only the late supplement is ours.
    free(s.frame_heap);s.frame_heap=NULL;
    s_frame_regions[AV_FRAME_REGIONS-1]=(av_frame_region_t){0};
    s.stripe[0]=s.stripe[1]=NULL; // borrowed from LVGL, returned by the caller
    free(s.inflate); s.inflate=NULL;
}
// Ask the session loop to reconnect on the id the menu selected. Returns false
// when there is nothing to do, e.g. the chosen channel is the one already on.
static bool request_switch(const char *id) {
    if (!id || !id[0]) return false;
    if (!strcmp(id,s.channel)) return false;   // already there; reconnecting would flicker
    unsigned count=atomic_load(&s.count);
    unsigned index=0;
    bool found=false;
    taskENTER_CRITICAL(&list_lock);
    for (unsigned i=0;i<count;i++) {
        if (!strcmp(s.list[i].id,id)) { index=i; found=true; break; }
    }
    if (found) atomic_store(&s.index,index);
    taskEXIT_CRITICAL(&list_lock);
    // Copy only an id that actually fits, so pending is always terminated. The
    // old bound could truncate a long id and leave the string unterminated.
    size_t length=strnlen(id,AV_CHANNEL_ID_MAX);
    if (!length || length>=AV_CHANNEL_ID_MAX) return false;
    memcpy(s.pending,id,length+1);
    atomic_store(&s.chosen_by_user,true);
    atomic_store(&s.switch_pending,true);
    xEventGroupSetBits(s.events,STOP);          // end this session; the loop reconnects
    return true;
}

// Applies one gesture. Called only from the session owner (main loop), so it can
// touch s.channel while no connection is being built.
static void process_key(const key_ev_t *item) {
    if (item->key==(bsp_btn_t)-1) {
        ESP_LOGI(TAG,"TV_UI view=%u selected=%u scroll=%u count=%u quality=%u focus=%u enabled=%u channel=%s",
            (unsigned)s.menu.view,s.menu.selected,s.menu.scroll,s.menu.count,
            s.menu.quality,s.menu.quality_selected,(unsigned)s.menu.quality_enabled,s.channel);
        return;
    }
    bool up = item->key==BSP_BTN_UP;

    // No gesture asks for a capture, and that is deliberate: every button on
    // this device already means something to the viewer, and taking one of them
    // for a diagnostic would change the product in order to serve the
    // debugging. A capture build takes its picture on its own, once a whole
    // frame has been drawn, and says so on the console.

    // The setup page owns the device while it is up. A channel must not change
    // and a session must not start behind it, so nothing else is reachable and
    // the only gesture that does anything is the one that leaves.
    if (atomic_load(&s_setup_active)) {
        if (item->key==BSP_BTN_OK && item->ev==BSP_BTN_LONG) {
            atomic_store(&s_setup_leave,true);
        }
        return;
    }

    // While the list, the status page or the backlight page is open the keys
    // belong to the overlay: a gesture must not both adjust something here and
    // change the channel behind it.
    if (s.menu.view!=UI_VIEW_VIDEO) {
        if (item->key==BSP_BTN_OK) {
            if (item->ev==BSP_BTN_CLICK) {
                if (s.menu.view==UI_VIEW_MENU || s.menu.view==UI_VIEW_QUALITY || s.menu.view==UI_VIEW_CONTROLS) {
                    ui_menu_confirm(&s.menu);
                } else {
                    ui_menu_close(&s.menu);
                }
            } else if (item->ev==BSP_BTN_LONG) {
                ui_menu_back(&s.menu);          // return one level, keeping channel position
            }
            return;
        }
        // The arrows adjust the backlight while that page is up, and put it away
        // on any other page. This is the one view where a click does not leave:
        // the whole point of opening it is to press up and down a few times, and
        // closing on the first press would make it useless.
        if (s.menu.view==UI_VIEW_BRIGHTNESS && item->ev==BSP_BTN_CLICK) {
            s.menu.brightness=av_brightness_step(s.menu.brightness,up);
            bsp_display_backlight(av_brightness_percent(s.menu.brightness));
            // Marked dirty rather than saved here: this runs on the button task,
            // and writing to flash takes long enough to delay the next press.
            // The session loop picks the flag up when it is otherwise idle.
            atomic_store(&s.settings_dirty,true);
            // Keep the page up while the viewer is still adjusting, the same way
            // the list refreshes its own timeout on every scroll.
            ui_menu_keep_open(&s.menu);
            return;
        }
        if (item->ev==BSP_BTN_CLICK) {
            if (s.menu.view==UI_VIEW_MENU || s.menu.view==UI_VIEW_QUALITY || s.menu.view==UI_VIEW_CONTROLS) {
                ui_menu_scroll_by(&s.menu,up?-1:1,menu_rows());
            }
            else ui_menu_close(&s.menu);         // any key leaves the other pages
            return;
        }
        // Leaving the status page for the setup screen. The arrows' long press is
        // the one gesture the status page does not already use -- OK's long press
        // closes the page, and both clicks scroll or close -- so the setup screen
        // is reachable without taking a control away from anything else.
        if (s.menu.view==UI_VIEW_STATUS && item->ev==BSP_BTN_LONG && up) {
            atomic_store(&s_setup_request,true);
            // The session has to actually stop, and the workers only notice when
            // this bit is set: they run `while (!stopping())`, so raising a
            // request flag alone leaves all three spinning and the session never
            // ends. Without this the drain waits out its full timeout and the
            // device restarts instead of showing the setup screen. Channel
            // switching sets the same bit for the same reason (request_switch).
            xEventGroupSetBits(s.events,STOP);
        }
        return;
    }

    if (item->key==BSP_BTN_OK) {
        if (item->ev==BSP_BTN_CLICK) {
            // The channel list. A click rather than a long press because the
            // list is what the viewer reaches for while watching; the cost is
            // that the button has to wait out the long-press window before the
            // driver can call it a click.
            ui_menu_set_rows(&s.menu,menu_rows());
            ui_menu_open_controls(&s.menu);
            return;
        }
        if (item->ev==BSP_BTN_LONG) {
            ft_tv_stop();
            return;
        }
        if (item->ev==BSP_BTN_DOUBLE) {
            // The backlight. It is set once and then left alone, so it takes the
            // gesture that responds immediately and is harder to reach by
            // accident -- holding a button for a second and a half is not
            // something a stray touch does.
            ui_menu_open_brightness(&s.menu);
            return;
        }
        return;
    }
    if (item->ev==BSP_BTN_LONG) {
        // Long press steps the volume, ten percent at a time.
        //
        // It was five, and that read as "the button does nothing": the codec
        // maps the percentage linearly onto -50..0 dB, so 5% is 2.5 dB, which is
        // about five steps of the ES8311's own register and very hard to hear.
        // Ten percent is 5 dB, which is a change the ear picks up. The register
        // was verified to move on every press, so this was always a step-size
        // problem rather than a broken control.
        //
        // The floor is low enough to be quiet in a small room and the ceiling
        // full scale, because the useful setting depends on the speaker and the
        // room and the viewer is the one who can hear it.
        if (up && s.volume<100) {
            s.volume = (uint8_t)(s.volume>90u ? 100u : s.volume+10u);
        }
        if (!up && s.volume>0) {
            s.volume = (uint8_t)(s.volume<10u ? 0u : s.volume-10u);
        }
        // Show the level. The audio worker picks the value up on its next chunk;
        // this only reports it. It runs even at the stop of the range, so the
        // indicator still appears and the button never looks dead.
        ui_menu_show_volume(&s.menu,s.volume);
        // Remember it, but not from here: see settings_dirty.
        atomic_store(&s.settings_dirty,true);
        return;
    }
    if (item->ev==BSP_BTN_DOUBLE) {
        // The status page needs a gesture of its own: a long press on either
        // arrow already means volume, and a long press on OK opens the list.
        ui_menu_open_status(&s.menu);
        return;
    }
    if (item->ev!=BSP_BTN_CLICK) return;
    // A click steps through the server-provided list. With no list yet (the
    // first CONFIG has not arrived) the gesture is ignored rather than guessed.
    // Work out the neighbouring channel and copy its id, all inside the lock the
    // receive task rewrites the table with. No pointer array is built: at
    // AV_CHANNEL_MAX entries that is a kilobyte on a 3.5 KB stack, and the
    // arithmetic it would feed is one line.
    char next[AV_CHANNEL_ID_MAX]={0};
    bool stepped=false;
    taskENTER_CRITICAL(&list_lock);
    unsigned count=atomic_load(&s.count);
    if (count) {
        unsigned index=atomic_load(&s.index) % count;
        unsigned target=(unsigned)(((int)index+(up?1:-1)+(int)count)%(int)count);
        const char *id=s.list[target].id;
        size_t n=id ? strnlen(id,AV_CHANNEL_ID_MAX) : 0;
        if (n>0 && n<AV_CHANNEL_ID_MAX) {
            memcpy(next,id,n+1);
            atomic_store(&s.index,target);
            stepped=true;
        }
    }
    taskEXIT_CRITICAL(&list_lock);
    if (!stepped) return;
    request_switch(next);
}
// Drain the key queue and carry out whatever the overlay asked for.
//
// Every loop that reads keys goes through here. The menu sets its action when a
// gesture is processed, and it must be collected in the same pass: doing that
// only in the session loop meant a choice made just as a session was ending was
// never picked up, so pressing OK appeared to do nothing at all.
// Returns true when the session must end so the new channel can be connected.
// `wait_ms` is how long to block when there is nothing to do. It must not be
// zero on the session path: that loop has no other blocking call, so a
// non-blocking read turns it into a spin that starves every other task on this
// single-core chip -- the watchdog then fires against whichever task happened to
// be running, and no data ever reaches the video and audio queues.
// Stop the current session and wait for its three workers to retire before the
// caller gives the session's buffers back. Returns false when a worker did not
// finish in time, in which case nothing may be freed.
//
// This is the whole channel-change contract in one place, because the failure it
// prevents is invisible when it is inline: the loop's exit condition is "all
// three workers signalled DONE", and each signals only after its last touch of a
// queue or a DMA buffer. free_session() destroys both, so ending the loop
// without this wait tore them down under a worker still inside an I2S write or a
// decode, and the device panicked and rebooted. Automatic channel changes never
// hit it because they let the session end on its own.
static void session_drain(void) {
    const EventBits_t want=RX_DONE|AUDIO_DONE|VIDEO_DONE;
    int64_t began=esp_timer_get_time();
    EventBits_t bits=xEventGroupWaitBits(s.events,want,pdFALSE,pdTRUE,
                                         pdMS_TO_TICKS(SESSION_DRAIN_MS));
    // The wait is normally over in well under SESSION_DRAIN_MS. A second, much
    // longer wait covers an unlucky overlap with a codec or DMA call; it is
    // logged because a channel change taking seconds is worth knowing about.
    if ((bits&want)!=want) {
        ESP_LOGW(TAG,"Workers still running after %u ms (missing=0x%02x); waiting up to %u ms more",
                 (unsigned)SESSION_DRAIN_MS,(unsigned)(want&~bits),
                 (unsigned)SESSION_DRAIN_GRACE_MS);
        bits=xEventGroupWaitBits(s.events,want,pdFALSE,pdTRUE,
                                 pdMS_TO_TICKS(SESSION_DRAIN_GRACE_MS));
    }
    if ((bits&want)==want) return;
    // Out of options. This session's queues and DMA buffers cannot be given back
    // while a worker is inside them, and there is no safe way to take a worker
    // out of a codec or DMA call from here: freeing them anyway is what crashed
    // the device, and leaving them allocated strands the memory, which starves
    // every later session until the screen is stuck blank. Restarting is the
    // only outcome that both keeps the memory consistent and leaves the device
    // usable -- it reconnects on its own, and the protected card identity is in
    // flash, untouched by a reset.
    //
    // Reached only if a worker outlives SESSION_DRAIN_MS+SESSION_DRAIN_GRACE_MS,
    // which no normal exit does.
    ESP_LOGE(TAG,"Workers did not retire after %u ms (missing=0x%02x); restarting",
             (unsigned)((esp_timer_get_time()-began)/1000),(unsigned)(want&~bits));
    vTaskDelay(pdMS_TO_TICKS(50));   // let the log reach the port
    esp_restart();
}
// Defined below, next to the loop that uses it. The setup screen drains keys
// through the same function, so that a gesture means the same thing whichever
// mode is up and there is only one place that reads the key queue.
static bool pump_keys(uint32_t wait_ms);


static bool pump_keys(uint32_t wait_ms) {
    key_ev_t item;
    bool first=true;
    while (xQueueReceive(s.keys,&item,first?pdMS_TO_TICKS(wait_ms):0)) {
        first=false;
        process_key(&item);
    }
    char chosen[AV_CHANNEL_ID_MAX];
    ui_action_t action=ui_menu_take_action(&s.menu,chosen,sizeof(chosen));
    if(action==UI_ACTION_QUALITY) {
        // Reuse the channel-switch drain contract even for the same channel.
        // The next HELLO and NVS write happen after all three workers stop.
        s.quality_pending=true;
        memcpy(s.pending,s.channel,sizeof(s.pending));
        atomic_store(&s.chosen_by_user,true);
        atomic_store(&s.switch_pending,true);
        ui_menu_banner(&s.menu,av_quality_name(s.menu.quality),NULL);
        xEventGroupSetBits(s.events,STOP);
        return true;
    }
    if (action==UI_ACTION_SWITCH) {
        if (request_switch(chosen)) {
            // Close the list first: the banner is drawn in the video view, and
            // leaving the menu up would hide the confirmation behind it.
            ui_menu_close(&s.menu);
            char name[UI_MENU_NAME_MAX];
            channel_name_of(chosen,name,sizeof(name));
            ui_menu_banner(&s.menu,name,NULL);
            ESP_LOGI(TAG,"Switching to %s",chosen);
            return true;
        }
        // The chosen channel is the one already playing: leave the list as it is
        // rather than appearing to do nothing.
    }
    return false;
}

bool ft_tv_connection_cancelled(void) {
    if(!s_refreshing)return true;
    pump_keys(0);
    if(stopping() || atomic_load(&s_setup_request))return true;
    if(!paint_connection_waiting_screen())return true;
    return stopping() || atomic_load(&s_setup_request);
}

// Owned by tv_task. FoloTalk has already paused LVGL, released its caches and
// closed the social link; the panel, codec and Wi-Fi are up.
static TaskHandle_t s_task;
static atomic_bool s_running;
static ft_tv_done_cb_t s_done;
static ft_tv_settings_cb_t s_settings;
static bool (*s_prepare)(void);

static player_t *allocate_player(void) {
    bool rtc=heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM)>=sizeof(player_t);
    player_t *p=rtc?heap_caps_calloc(1,sizeof(player_t),MALLOC_CAP_RTCRAM|MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT):NULL;
    if(!p) {
        rtc=false;
        p=heap_caps_calloc(1,sizeof(player_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA);
    }
    if(!p)return NULL;
    // Only the small palette is touched once per pixel. Keep it in the fast
    // SRAM domain while channel/menu strings and session state use RTC RAM.
    p->palette=heap_caps_calloc(AV_PALETTE_ENTRIES,sizeof(*p->palette),
                               MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA);
    if(!p->palette){free(p);return NULL;}
    ESP_LOGI(TAG,"PLAYER_WORKSPACE rtc=%u state_bytes=%u palette_bytes=%u",
             (unsigned)rtc,(unsigned)sizeof(*p),(unsigned)(AV_PALETTE_ENTRIES*sizeof(*p->palette)));
    return p;
}

static void release_player(void) {
    atomic_store(&heap_media_phase,false);
    (void)heap_caps_register_failed_alloc_callback(s_host_allocation_failed?s_host_allocation_failed:tv_heap_ignore);
    if (!s_player) return;
    taskENTER_CRITICAL(&player_lock);
    player_t *p=s_player;
    s_player=NULL;              // the button task now sees no player
    taskEXIT_CRITICAL(&player_lock);
    if (p->keys) vQueueDelete(p->keys);
    if (p->events) vEventGroupDelete(p->events);
    free(p->palette);
    free(p);
}

static void tv_task(void *arg) {
    (void)arg;
    if(s_prepare && !s_prepare()) {
        ESP_LOGW(TAG,"Caller workspace handoff failed");
        goto player_finished;
    }
    ESP_LOGW(TAG,"TV start heap=%u largest=%u",(unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    memset(s.channel,0,sizeof(s.channel));
    memset(s.pending,0,sizeof(s.pending));
    atomic_store(&s.count,0u);
    atomic_store(&s.index,0u);
    atomic_store(&s.switch_pending,false);
    atomic_store(&s.list_dirty,false);
    av_channel_policy_reset(&s.proven);
    s.dead_streak=0;
    waiting_drawn=false; bars_drawn=false;
    menu_overlay.valid=false;
    shown_channel[0]=0; last_attempt_addr[0]=0;
    ui_menu_init(&s.menu);
    quality_load();
    s.menu.quality=s_quality;
    atomic_store(&s.quality_available,false);
    s.menu.brightness=av_brightness_nearest_index(s_brightness);
    atomic_store(&s.battery_soc,bsp_battery_soc());
    xEventGroupClearBits(s.events,0xFFFFFF);
    xEventGroupSetBits(s.events,WIFI_READY);
    av_server_addr_t target;
    bool server_configured=server_address(&target);
    (void)server_configured;
    char last_announced[UI_MENU_NAME_MAX];
    memset(last_announced,0,sizeof(last_announced));
    while(true) {
        pump_keys(50);
        if(atomic_load(&s_setup_request))break;
        if(s.quality_pending) {
            s.quality_pending=false;
            s_quality=s.menu.quality;
            quality_save();
            ESP_LOGI(TAG,"Quality %s",av_quality_id(s_quality));
        }
        // Apply a requested channel before opening the next session, so the
        // handshake names the new channel. Clearing the flag here (not in the
        // key handler) keeps exactly one writer of s.channel.
        if (atomic_exchange(&s.switch_pending,false)) {
            ESP_LOGI(TAG,"Channel %s -> %s",s.channel,s.pending);
            memcpy(s.channel,s.pending,sizeof(s.channel));
        }
        xEventGroupClearBits(s.events,STOP|RX_DONE|AUDIO_DONE|VIDEO_DONE|CONFIG_READY|CLOCK_READY);
        // Rousing the connection is the WiFi stack's job now, not this loop's.
        // The component retries a dropped association a few times and then
        // rescans on a backoff, so calling esp_wifi_connect() here as well would
        // race it. This waits for the address instead, and keeps the keys alive
        // while it does so the viewer is never locked out of the device.
        if(!(xEventGroupGetBits(s.events)&WIFI_READY)) {
            int64_t deadline=esp_timer_get_time()+15000000;
            while(!(xEventGroupGetBits(s.events)&WIFI_READY) && esp_timer_get_time()<deadline && !stopping()) {
                pump_keys(50);
            }
            if(!(xEventGroupGetBits(s.events)&WIFI_READY) || stopping()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        }
        s.submitted_samples=s.decoded=s.dropped=s.decode_max_ms=s.feed_gap_max_ms=0;
        s.silence_samples=0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        atomic_store(&s.rx_cpu_us,0u); atomic_store(&s.audio_cpu_us,0u);
        atomic_store(&s.video_cpu_us,0u);
        atomic_store(&s.housekeeping_us,0u);
#endif
        atomic_store(&s.inflate_us,0u); atomic_store(&s.panel_us,0u);
        atomic_store(&s.panel_frames,0u);
        atomic_store(&s.panel_wait_us,0u);
        atomic_store(&s.expand_us,0u); atomic_store(&s.enlarge_us,0u);
        atomic_store(&s.overlay_us,0u); atomic_store(&s.submit_us,0u);
        atomic_store(&s.inflate_bytes,0u);
        atomic_store(&s.rx_video_bytes,0u); atomic_store(&s.rx_video_packets,0u);
        atomic_store(&s.rx_audio_packets,0u);
        atomic_store(&s.fps_x10,0u); atomic_store(&s.rx_bytes_per_s,0u);
        bars_drawn=false;   // the first frame blackens the bars again
        atomic_store(&s.rx_io_us,0u); atomic_store(&s.rx_wait_us,0u);
        atomic_store(&s.rx_overhead_us,0u); atomic_store(&s.rx_iterations,0u);
        for(unsigned k=0;k<RX_PHASES;k++) {
            atomic_store(&s.rx_timing[k].total_us,0u);atomic_store(&s.rx_timing[k].max_us,0u);
        }
        s.rx_stall_us=0;memset(s.rx_stall_parts,0,sizeof(s.rx_stall_parts));
        atomic_store(&s.rx_frame_starts,0u); atomic_store(&s.rx_frame_skipped,0u);
        atomic_store(&s.rx_stray_packets,0u); atomic_store(&s.rx_max_stripes,0u);
        atomic_store(&s.rx_nobuf_packets,0u);
        atomic_store(&s.rx_header_gap_max_ms,0u);
        atomic_store(&heap_media_phase,false);
        atomic_store(&s.frame_ready,0u); atomic_store(&s.frame_overflow,0u);
        atomic_store(&s.frame_size_max,0u);
        atomic_store(&s.frame_receive_max_us,0u); atomic_store(&s.frame_draw_max_us,0u);
        atomic_store(&s.frame_submit_gap_max_us,0u); atomic_store(&s.menu_frames,0u);
        s.audio_high=s.video_high=0; s.clock_us=0;
        atomic_store(&s.media_started,false);
        if(s_connection_provider)s.stripe[0]=s_lvgl_stripes; // borrowed; no media allocation
        esp_err_t connection_err=refresh_connection();
        if(atomic_load(&s_setup_request))break;
        if(stopping())continue; // channel/quality changed while refreshing
        if(connection_err!=ESP_OK) {
            atomic_store(&s.link,(int)LINK_DOWN);
            ESP_LOGW(TAG,"Connection refresh failed"); // No remote strings or credentials.
            for(unsigned i=0;i<20 && !atomic_load(&s_setup_request) && !stopping();i++) {
                pump_keys(100);
                if(!stopping() && !paint_connection_waiting_screen())break;
            }
            continue; // never allocate or connect using credentials from a prior session
        }
        // Back to "dialling", but only when that is worth saying.
        //
        // Unconditionally would be wrong twice over. It would make the screen
        // claim a fresh attempt every second on a device that is failing, and --
        // because the state is reset the instant a session starts and the
        // failure is only concluded once the dial gives up -- the screen would
        // never get to show the failure at all. It would settle on whichever
        // value happened to be set when it last painted, which is "connecting",
        // for as long as the device kept retrying.
        //
        // So the state returns to "dialling" only when the address is not the one
        // just tried: the first attempt, or a change the viewer made. A retry of
        // the same address leaves the concluded failure on screen, where it
        // belongs, until something is actually different.
        {
            av_server_addr_t probe;
            char text[AV_SERVER_ADDR_MAX+8]={0};
            if (server_address(&probe)) {
                snprintf(text,sizeof(text),"%s:%u",probe.host,(unsigned)probe.port);
            }
            if (strcmp(text,last_attempt_addr)) {
                snprintf(last_attempt_addr,sizeof(last_attempt_addr),"%s",text);
                atomic_store(&s.link,(int)LINK_TRYING);
            }
        }
        s.last_packet_ms=(uint32_t)(esp_timer_get_time()/1000);
        if(!allocate_session()) {
            ESP_LOGE(TAG,"Session allocation failed free=%u largest=%u",(unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            free_session(); vTaskDelay(pdMS_TO_TICKS(2000)); continue;
        }
        // The sizes are computed from the same constants the allocations above
        // used, not written in.
        //
        // They read "JPEG=49152 PCM=12800 stripes=20480" for a long time after
        // the allocations behind them changed -- 49152 was three 16384-byte
        // buffers when a frame could be split, and the real figures are now
        // 2 x AV_VIDEO_MAX = 45056 for the picture buffers, 24 x AV_AUDIO_BYTES
        // = 7776 of ADPCM payload (30720 when the queue held PCM), and
        // STRIPE_BYTES x 2 = 20480. An external
        // review noticed the line was stale twice before anyone corrected it.
        // A log line that reports numbers nobody computed is worse than no
        // line: it is read as a measurement.
        ESP_LOGI(TAG,"Allocated complete-frame pool=%u audio(ADPCM)=%ux%u=%u stripes=%ux%u=%u; heap=%u largest=%u",
            (unsigned)s.frame_ring.capacity,
            (unsigned)PCM_QUEUE,(unsigned)AV_AUDIO_BYTES,
            (unsigned)(PCM_QUEUE*AV_AUDIO_BYTES),
            (unsigned)STRIPE_BUFFERS,(unsigned)STRIPE_BYTES,
            (unsigned)(STRIPE_BUFFERS*STRIPE_BYTES),
            (unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        // The receiver outranks the video task, and that ordering is load bearing
        // rather than a preference.
        //
        // It used to be the other way round, and the reason was a real hazard
        // that does not apply to this code. The receiver hands out the buffers
        // the video task hands back, so if the receiver can wait for one while
        // outranking the task that would free it, it deadlocks. But the wait
        // here has a zero timeout -- `xQueueReceive(s.free_video,&v.jpeg,0)` --
        // and a receiver that finds no buffer discards that packet and carries
        // on. It never blocks on a buffer, so it can never hold the video task
        // off the processor, and the inversion cannot happen.
        //
        // What the old ordering did cause is the opposite fault, and it is what
        // the picture has been fighting. There is one core. The video task
        // inflates a stripe with the processor held for up to 146 ms at a time
        // (decode_max_ms, measured), and while it does, a receiver below it does
        // not run at all -- so nothing reads the socket. The server's writes
        // stop being drained, its audio falls behind by exactly that much, and
        // the device reports an audio underrun and drops the session. Measured
        // with the picture running, the device counted 317 to 361 of the 500
        // audio packets the server sent in the same ten seconds; with the sound
        // alone on the same channel and the same seven minutes, 40 of 41
        // intervals were perfect. The bytes are lost because nothing is reading,
        // not because the link cannot carry them.
        //
        // So: receiver 6, video 5. The receiver spends its time blocked in
        // select() when the link is quiet, which leaves the processor to the
        // video task; when bytes do arrive it takes priority, drains them, and
        // goes back to waiting. Audio stays above both at 7 because the sound is
        // the timeline everything else is paced against.
        shot_maybe_request();
        if(xTaskCreate(video_task,"av_video",4096,NULL,5,NULL)!=pdPASS) { fail("video task allocation"); xEventGroupSetBits(s.events,VIDEO_DONE); }
        // One chunk of PCM now lives on this stack beside the queued ADPCM block,
        // 324 bytes more than the PCM-only layout needed, so the stack grows a
        // kilobyte; the queue gave back about 23 KB. The high-water mark is
        // logged when the task ends -- trim this to what that says.
        if(xTaskCreate(audio_task,"av_audio",5120,NULL,7,NULL)!=pdPASS) { fail("audio task allocation"); xEventGroupSetBits(s.events,AUDIO_DONE); }
        if(xTaskCreate(receive_task,"av_rx",5120,NULL,6,NULL)!=pdPASS) { fail("receive task allocation"); xEventGroupSetBits(s.events,RX_DONE); }
        int64_t session_start=esp_timer_get_time();
        int64_t next_metrics=session_start+10000000;
        int64_t next_battery=session_start;
        int64_t rate_at=session_start, next_rate=session_start+1000000;
        uint32_t rate_decoded=0, rate_bytes=0;
        int64_t last_tick=session_start;
        // The banner fires once per session, at the moment the server confirms
        // the channel; last_announced carries across sessions so reconnecting to
        // the same channel does not raise it again.
        bool announced=false;
        uint32_t last_decoded=0;
        uint32_t phase_previous[RX_PHASES]={0};
        // Where the frame time went, as of the previous report, so each interval
        // can say what this ten seconds cost rather than what the session has
        // cost so far.
        uint32_t last_inflate_us=0, last_panel_us=0, last_inflate_bytes=0;
        uint32_t last_panel_frames=0, last_panel_wait=0;
        uint32_t last_expand_us=0, last_enlarge_us=0, last_overlay_us=0,
                 last_submit_us=0;
        uint32_t last_rx_bytes=0, last_rx_pkts=0, last_rx_audio=0;
        uint32_t last_rx_io=0, last_rx_wait=0, last_rx_iters=0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        uint32_t last_rx_cpu=0, last_video_cpu=0, last_audio_cpu=0;
        uint32_t last_idle_cpu=(uint32_t)ulTaskGetIdleRunTimeCounter();
#endif
        while((xEventGroupGetBits(s.events)&(RX_DONE|AUDIO_DONE|VIDEO_DONE))!=(RX_DONE|AUDIO_DONE|VIDEO_DONE)) {
            // A channel was chosen: end this session and reconnect on the new
            // one. request_switch() has already set STOP, so the workers are on
            // their way out; session_drain() waits for them before the buffers
            // are reclaimed below.
            bool switching=pump_keys(50);
            // A setup request ends the session the same way a channel change
            // does, and for a stronger reason: the access point needs the memory
            // this session is holding.
            if(switching || atomic_load(&s_setup_request)) {
                session_drain();
                break;
            }

            // Install the channel list the receiver just parsed, here, where no
            // frame is being drawn from it. Built straight from s.list so the
            // table is not held in a third copy: on this chip the per-entry cost
            // is what decides how many channels fit at all.
            // Rebuilding the list resets its highlight and scroll, so it must not
            // happen while the viewer is looking at it: a CONFIG arrives on every
            // reconnect, and one landing mid-browse would yank the list back to
            // the top. Deferred until the overlay is closed.
            if (atomic_load(&s.list_dirty) && s.menu.view==UI_VIEW_VIDEO) {
                // The staging copy is heap, not stack: at a few hundred channels
                // it is far larger than this task's stack, and it is needed for
                // only the instant the overlay is rebuilt.
                ui_menu_entry_t *entries=heap_caps_malloc(sizeof(ui_menu_entry_t)*AV_CHANNEL_MAX,
                                                          MALLOC_CAP_8BIT);
                if (entries) {
                    atomic_store(&s.list_dirty,false);
                    unsigned n;
                    taskENTER_CRITICAL(&list_lock);
                    n=atomic_load(&s.count);
                    for (unsigned i=0;i<n;i++) {
                        memset(&entries[i],0,sizeof(entries[i]));
                        memcpy(entries[i].id,s.list[i].id,sizeof(entries[i].id));
                        memcpy(entries[i].name,s.list[i].name,sizeof(entries[i].name));
                    }
                    taskEXIT_CRITICAL(&list_lock);
                    if(atomic_load(&s.quality_available))ui_menu_enable_quality(&s.menu,s_quality);
                    if (n) ui_menu_set_rows(&s.menu,menu_rows());
                if (n && !ui_menu_load(&s.menu,entries,n,s.channel)) {
                        ESP_LOGW(TAG,"channel list rejected by the overlay; keeping the previous one");
                    }
                    free(entries);
                } else {
                    ESP_LOGW(TAG,"No memory to rebuild the channel overlay");
                }
            }

            // Drive the overlay clock from the session loop, which is the only
            // place that sees every tick even while the video task is busy.
            int64_t tick_now=esp_timer_get_time();
            if (ui_menu_tick(&s.menu,(tick_now-last_tick)/1000)) {
                // The banner or the page expired; nothing else needed here --
                // the next frame simply stops drawing it.
            }
            last_tick=tick_now;
            // The gauge is on the shared I2C bus; read it rarely and never in
            // the frame path.
            if(tick_now>=next_battery) {
                int soc=bsp_battery_soc();
                atomic_store(&s.battery_soc,soc);   // -1 when unreadable
                next_battery=tick_now+30000000;     // every 30 s
            }
            // Frame rate and receive rate over the last second. The counters are
            // zeroed at the start of each session, as are these baselines, so
            // the differences never wrap. Audio is counted at its fixed packet
            // size: the receive counter only tallies video bytes.
            if(tick_now>=next_rate) {
                uint32_t span_ms=(uint32_t)((tick_now-rate_at)/1000);
                uint32_t decoded_now=s.decoded;
                uint32_t bytes_now=atomic_load(&s.rx_video_bytes)
                    +atomic_load(&s.rx_audio_packets)*(uint32_t)AV_AUDIO_BYTES;
                if(span_ms) {
                    atomic_store(&s.fps_x10,
                        (uint32_t)((uint64_t)(decoded_now-rate_decoded)*10000u/span_ms));
                    atomic_store(&s.rx_bytes_per_s,
                        (uint32_t)((uint64_t)(bytes_now-rate_bytes)*1000u/span_ms));
                }
                rate_decoded=decoded_now; rate_bytes=bytes_now;
                rate_at=tick_now; next_rate=tick_now+1000000;
            }
            // Write back a setting that changed. Done here rather than in the key
            // handler because a flash write takes long enough to be felt as a
            // laggy button, and this loop is the one place with time to spare.
            // The volume is read again at the moment of writing, so a burst of
            // presses settles on the value the viewer stopped at instead of
            // writing each intermediate one.
            if (atomic_exchange(&s.settings_dirty,false)) {
                s_brightness=av_brightness_percent(s.menu.brightness);
                if(s_settings) s_settings(s.volume,s_brightness);
            }
            // Announce the channel once the server has confirmed which one it is
            // serving, not when the key was pressed: the banner then names what
            // is actually on screen, which is what the viewer wants to check.
            if(!announced && (xEventGroupGetBits(s.events)&CONFIG_READY)) {
                announced=true;
                char name[UI_MENU_NAME_MAX];
                current_channel_name(name,sizeof(name));
                if (strcmp(name,last_announced)) {
                    ui_menu_banner(&s.menu,name,last_announced);
                    strncpy(last_announced,name,sizeof(last_announced)-1);
                    last_announced[sizeof(last_announced)-1]='\0';
                }
            }
            // Read producer timestamp BEFORE now: the receiver may preempt this
            // lower-priority task; unsigned now-old subtraction otherwise wraps
            // when a freshly updated timestamp is newer than the sampled now.
            uint32_t last_packet=atomic_load(&s.last_packet_ms);
            int64_t now=esp_timer_get_time();
            // Before the first media packet the server is still filling its
            // transcode buffer, which takes seconds on a live channel. Only
            // after media starts flowing does a 5 s gap mean a stalled stream.
            uint32_t idle_limit=atomic_load(&s.media_started)
                ? 5000u : AV_FIRST_MEDIA_TIMEOUT_MS;
            if(!stopping() && !rx_done() && av_elapsed_ms((uint32_t)(now/1000),last_packet)>idle_limit) {
                ESP_LOGW(TAG,"RX_INACTIVE age_ms=%"PRIu32,av_elapsed_ms((uint32_t)(now/1000),last_packet));
                fail("receive inactivity; cooperative stop");
            }
            if(now>=next_metrics) {
                uint32_t frames=s.decoded;
                uint32_t interval_ms=(uint32_t)((now-(next_metrics-10000000))/1000);
                uint32_t inflate_us=atomic_load(&s.inflate_us)-last_inflate_us;
                uint32_t panel_us=atomic_load(&s.panel_us)-last_panel_us;
                uint32_t panel_frames=atomic_load(&s.panel_frames)-last_panel_frames;
                uint32_t panel_wait=atomic_load(&s.panel_wait_us)-last_panel_wait;
                uint32_t in_bytes=atomic_load(&s.inflate_bytes)-last_inflate_bytes;
                // Bytes per second, not per frame: it is the number that says
                // whether the link or the processor is the limit, and it is
                // comparable across channels however their contents compress.
                uint32_t rate=interval_ms ? (uint32_t)((uint64_t)in_bytes*1000/interval_ms) : 0;
                uint32_t rx_bytes=atomic_load(&s.rx_video_bytes)-last_rx_bytes;
                uint32_t rx_pkts=atomic_load(&s.rx_video_packets)-last_rx_pkts;
                uint32_t rx_audio=atomic_load(&s.rx_audio_packets)-last_rx_audio;
                uint32_t rx_rate=interval_ms ? (uint32_t)((uint64_t)rx_bytes*1000/interval_ms) : 0;
                uint32_t rx_io=atomic_load(&s.rx_io_us)-last_rx_io;
                uint32_t rx_wait=atomic_load(&s.rx_wait_us)-last_rx_wait;
                uint32_t rx_iters=atomic_load(&s.rx_iterations)-last_rx_iters;
                // The stripe's cost, split. `parts_sum` is not a measurement:
                // it is the four parts added up on the way out, so that a
                // reader can see at a glance whether they account for
                // `panel_us`. A sum larger than the interval that contains it
                // means a timer is broken, and every mean derived from it is
                // wrong -- which is the failure this project has already had
                // once, when `panel_us` carried milliseconds into a field
                // divided by a thousand a second time.
                uint32_t expand_us=atomic_load(&s.expand_us)-last_expand_us;
                uint32_t enlarge_us=atomic_load(&s.enlarge_us)-last_enlarge_us;
                uint32_t overlay_us=atomic_load(&s.overlay_us)-last_overlay_us;
                uint32_t submit_us=atomic_load(&s.submit_us)-last_submit_us;
                uint32_t parts_sum_us=inflate_us+expand_us+enlarge_us
                    +overlay_us+submit_us+panel_wait;
                // video_in_bps against video_use_bps is the whole diagnosis: the
                // first is what the link delivered, the second what the drawing
                // path consumed. A wide gap with a low first number is a starved
                // link; a first number near the ceiling with a small second is a
                // decoder that cannot keep up.
                ESP_LOGD(TAG,"CLOCK_ESTIMATED interval_frames=%"PRIu32" interval_ms=%"PRIu32
                    " dropped=%"PRIu32" queue_high=%"PRIu32"/%"PRIu32" heap=%u largest=%u decode_max_ms=%"PRIu32
                    " inflate_ms=%"PRIu32" panel_ms=%"PRIu32" panel_frames=%"PRIu32" panel_wait_ms=%"PRIu32" in_bytes=%"PRIu32" in_bps=%"PRIu32
                    " expand_ms=%"PRIu32" enlarge_ms=%"PRIu32" overlay_ms=%"PRIu32" submit_ms=%"PRIu32" parts_ms=%"PRIu32
                    " rx_pkts=%"PRIu32" rx_bps=%"PRIu32" rx_audio=%"PRIu32" rssi=%d"
                    " io_ms=%"PRIu32" wait_ms=%"PRIu32" iters=%"PRIu32" per_iter_us=%"PRIu32
                    " starts=%"PRIu32" late=%"PRIu32" stray=%"PRIu32" maxstripes=%"PRIu32 " nobuf=%"PRIu32
                    " hdrgap_max=%"PRIu32" phy=%s ch=%u",
                    frames-last_decoded,interval_ms,
                    (uint32_t)s.dropped,(uint32_t)s.audio_high,(uint32_t)s.video_high,
                    (unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    (uint32_t)atomic_exchange(&s.decode_max_ms,0u),
                    inflate_us/1000,panel_us/1000,panel_frames,panel_wait/1000,in_bytes,rate,
                    expand_us/1000,enlarge_us/1000,overlay_us/1000,submit_us/1000,
                    parts_sum_us/1000,
                    rx_pkts,rx_rate,rx_audio,wifi_rssi(),
                    rx_io/1000,rx_wait/1000,rx_iters,
                    rx_iters ? (uint32_t)(rx_io/rx_iters) : 0,
                    (uint32_t)atomic_load(&s.rx_frame_starts),
                    (uint32_t)atomic_load(&s.rx_frame_skipped),
                    (uint32_t)atomic_load(&s.rx_stray_packets),
                    (uint32_t)atomic_exchange(&s.rx_max_stripes, 0u),
                    (uint32_t)atomic_exchange(&s.rx_nobuf_packets, 0u),
                    (uint32_t)atomic_exchange(&s.rx_header_gap_max_ms, 0u),
                    wifi_phy(),wifi_channel());
                ESP_LOGI(TAG,"FRAME_GATE frames=%"PRIu32" interval_ms=%"PRIu32
                    " ready=%"PRIu32" overflow=%"PRIu32
                    " capacity=%u frame_bytes_max=%"PRIu32" receive_max_ms=%"PRIu32" draw_max_ms=%"PRIu32
                    " submit_gap_max_ms=%"PRIu32" menu_frames=%"PRIu32,
                    frames-last_decoded,interval_ms,
                    (uint32_t)atomic_load(&s.frame_ready),(uint32_t)atomic_load(&s.frame_overflow),
                    (unsigned)s.frame_ring.capacity,
                    (uint32_t)atomic_exchange(&s.frame_size_max,0u),
                    (uint32_t)atomic_exchange(&s.frame_receive_max_us,0u)/1000u,
                    (uint32_t)atomic_exchange(&s.frame_draw_max_us,0u)/1000u,
                    (uint32_t)atomic_exchange(&s.frame_submit_gap_max_us,0u)/1000u,
                    (uint32_t)atomic_exchange(&s.menu_frames,0u));
                last_decoded=frames; last_inflate_us+=inflate_us;
                last_panel_us+=panel_us; last_inflate_bytes+=in_bytes;
                last_panel_frames+=panel_frames; last_panel_wait+=panel_wait;
                last_rx_bytes+=rx_bytes; last_rx_pkts+=rx_pkts; last_rx_audio+=rx_audio;
                last_rx_io+=rx_io; last_rx_wait+=rx_wait; last_rx_iters+=rx_iters;
                uint32_t phase_ms[RX_PHASES],phase_max_ms[RX_PHASES],stall,stall_parts[RX_PHASES];
                for(unsigned k=0;k<RX_PHASES;k++) {
                    uint32_t total=atomic_load(&s.rx_timing[k].total_us);
                    phase_ms[k]=(total-phase_previous[k])/1000;phase_previous[k]=total;
                    phase_max_ms[k]=atomic_exchange(&s.rx_timing[k].max_us,0u)/1000;
                }
                taskENTER_CRITICAL(&clock_lock);
                stall=s.rx_stall_us;s.rx_stall_us=0;
                memcpy(stall_parts,s.rx_stall_parts,sizeof(stall_parts));
                taskEXIT_CRITICAL(&clock_lock);
                ESP_LOGI(TAG,"RX_WAIT interval_ms=%"PRIu32" socket_select_ms=%"PRIu32
                    " recv_call_ms=%"PRIu32" audio_queue_ms=%"PRIu32" frame_ring_ms=%"PRIu32" frame_queue_ms=%"PRIu32
                    " max_ms=%"PRIu32"/%"PRIu32"/%"PRIu32"/%"PRIu32"/%"PRIu32,
                    interval_ms,phase_ms[0],phase_ms[1],phase_ms[2],phase_ms[3],phase_ms[4],
                    phase_max_ms[0],phase_max_ms[1],phase_max_ms[2],phase_max_ms[3],phase_max_ms[4]);
                ESP_LOGI(TAG,"RX_STALL gap_ms=%"PRIu32" socket_select_ms=%"PRIu32
                    " recv_call_ms=%"PRIu32" audio_queue_ms=%"PRIu32" frame_ring_ms=%"PRIu32" frame_queue_ms=%"PRIu32,
                    stall/1000,stall_parts[0]/1000,stall_parts[1]/1000,stall_parts[2]/1000,stall_parts[3]/1000,stall_parts[4]/1000);
                ESP_LOGI(TAG,"HEAP_RX failures=%"PRIu32" after_config=%"PRIu32" last_bytes=%"PRIu32" caps=%"PRIu32,
                    (uint32_t)atomic_load(&heap_failures),(uint32_t)atomic_load(&heap_media_failures),
                    (uint32_t)atomic_load(&heap_failed_bytes),(uint32_t)atomic_load(&heap_failed_caps));
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
                uint32_t rx_cpu=atomic_load(&s.rx_cpu_us), video_cpu=atomic_load(&s.video_cpu_us);
                uint32_t audio_cpu=atomic_load(&s.audio_cpu_us);
                uint32_t idle_cpu=(uint32_t)ulTaskGetIdleRunTimeCounter();
                ESP_LOGI(TAG,"CPU_RUNTIME interval_ms=%"PRIu32" video_ms=%"PRIu32
                    " rx_ms=%"PRIu32" audio_ms=%"PRIu32" idle_ms=%"PRIu32,
                    interval_ms,(video_cpu-last_video_cpu)/1000,(rx_cpu-last_rx_cpu)/1000,
                    (audio_cpu-last_audio_cpu)/1000,(idle_cpu-last_idle_cpu)/1000);
                last_rx_cpu=rx_cpu; last_video_cpu=video_cpu; last_audio_cpu=audio_cpu; last_idle_cpu=idle_cpu;
#endif
#if LWIP_STATS && TCP_STATS && LINK_STATS
                // Cumulative stack counters distinguish receive-side drops and
                // allocation pressure from losses before packets reach lwIP.
                ESP_LOGI(TAG,"TCP_RX_STATS recv=%u drop=%u memerr=%u err=%u "
                    "chkerr=%u link_drop=%u link_memerr=%u",
                    (unsigned)lwip_stats.tcp.recv,(unsigned)lwip_stats.tcp.drop,
                    (unsigned)lwip_stats.tcp.memerr,(unsigned)lwip_stats.tcp.err,
                    (unsigned)lwip_stats.tcp.chkerr,(unsigned)lwip_stats.link.drop,
                    (unsigned)lwip_stats.link.memerr);
#endif
                last_expand_us+=expand_us; last_enlarge_us+=enlarge_us;
                last_overlay_us+=overlay_us; last_submit_us+=submit_us;
                next_metrics=now+10000000;
            }
        }
        // Workers signal only after their final resource access; no forced deletion.
        uint32_t session_ms=(esp_timer_get_time()-session_start)/1000;
        uint32_t fps_x1000=session_ms ? (uint64_t)s.decoded*1000000/session_ms : 0;
        ESP_LOGD(TAG,"CLOCK_ESTIMATED rendered_fps_x1000=%"PRIu32" session_ms=%"PRIu32
            " heap=%u largest=%u",fps_x1000,session_ms,(unsigned)esp_get_free_heap_size(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        // Three quantities, named, because their differences are the thing
        // that was wrong: what played, how much of it this device made up, and
        // the program position the picture is judged against. Only the last is
        // comparable with a packet's timestamp.
        uint32_t submitted=s.submitted_samples, silence=s.silence_samples;
        uint32_t program=submitted>silence?submitted-silence:0u;
        ESP_LOGD(TAG,"ESTIMATED clock: submitted_samples=%"PRIu32" of_which_silence=%"PRIu32
            " program_samples=%"PRIu32" program_ms=%"PRIu32
            " DMA_completed=unmeasured acoustic=unmeasured decoded=%"PRIu32
            " dropped=%"PRIu32" decode_max_ms=%"PRIu32" feed_gap_max_ms=%"PRIu32" queue_high=%"PRIu32"/%"PRIu32" min_heap=%u",
            submitted,silence,program,(uint32_t)((int64_t)program*1000/16000),
            s.decoded,s.dropped,s.decode_max_ms,s.feed_gap_max_ms,s.audio_high,s.video_high,
            (unsigned)esp_get_minimum_free_heap_size());
        // Safe only because the workers were waited for above: they signal DONE
        // after their last touch of a queue or a DMA buffer, and this destroys
        // both.
        free_session();

        // Answered here rather than inside the session loop because this is the
        // point where the session's memory is free again: the access point, its
        // web server and its DNS responder are built out of exactly that memory,
        // and starting them while it was still held is what would make them fail.
        if(atomic_load(&s_setup_request)) break;

        // A session that never produced a picture means the source is broken, not
        // that the network hiccuped: live sources fail fast with a 403 or a dead
        // host. Step to the next channel after a couple of tries so a dead entry
        // in the list cannot trap the viewer on the test pattern.
        //
        // Not when the server was never reached, though. That is not a fault of
        // the channel: every channel fails identically, so stepping would cycle
        // the whole table for nothing while the viewer watches the name change
        // and never sees a picture. Staying put keeps the waiting screen up --
        // which is the screen the status page is drawn over, and that page is
        // the only way from here to the setup screen where the address is fixed.
        //
        // And not for a channel that has produced a picture before. That is the
        // case a count of fruitless sessions cannot see on its own: a source
        // that stutters ends a session with no picture just as a dead one does,
        // so a working channel was being abandoned after two bad moments and the
        // viewer's choice discarded with it. The set lives in av_channel_policy,
        // where the rule can be tested.
        //
        // Read fresh rather than from the copy taken at start-up, so an address
        // stored on the setup page is picked up on the next pass without a
        // restart.
        if (atomic_load(&s.switch_pending)) {
            // A channel switch was requested. The session that just ended was
            // interrupted by the user; it did not fail on its own.
            // Do not evaluate dead-channel skip for the old channel, and preserve
            // s.chosen_by_user so the NEW channel gets its user-chosen allowance.
            s.dead_streak = 0;
        } else {
            // Only a session that got through to the server and then saw nothing
            // counts as a bad channel. A session that never reached the server says
            // nothing about the channel at all.
            const bool server_answered = atomic_load(&s.link)==(int)LINK_UP;
            unsigned limit = atomic_load(&s.chosen_by_user)
                ? DEAD_CHANNEL_TRIES_USER : DEAD_CHANNEL_TRIES;
            if (av_channel_policy_should_skip(&s.proven,s.channel,server_answered,
                                              atomic_load(&s.media_started),&s.dead_streak,
                                              limit,atomic_load(&s.count))) {
                atomic_store(&s.chosen_by_user,false);
                unsigned count=atomic_load(&s.count);
                unsigned index=(atomic_load(&s.index)+1)%count;
                char next[AV_CHANNEL_ID_MAX]={0};
                bool ok=false;
                taskENTER_CRITICAL(&list_lock);
                const char *id=s.list[index].id;
                size_t length=id ? strnlen(id,AV_CHANNEL_ID_MAX) : 0;
                if (length>0 && length<AV_CHANNEL_ID_MAX) {
                    memcpy(next,id,length+1);
                    atomic_store(&s.index,index);
                    ok=true;
                }
                taskEXIT_CRITICAL(&list_lock);
                if (ok) {
                    // Only a channel that has never shown a picture reaches here,
                    // so the message can say that rather than the weaker "no
                    // picture just now" -- which would also describe the stutter
                    // this path deliberately does not act on.
                    ESP_LOGW(TAG,"%s has never shown a picture; trying %s",s.channel,next);
                    // request_switch() raises chosen_by_user because it cannot tell
                    // a key press from this call. Undo it here: nobody asked for
                    // this channel, so it gets the automatic allowance, not the
                    // wider one a deliberate choice earns.
                    request_switch(next);
                    atomic_store(&s.chosen_by_user,false);
                }
            } else if (atomic_load(&s.media_started)) {
                atomic_store(&s.chosen_by_user,false);
            }
        }
        if(atomic_load(&s_setup_request)) break;
        for(int i=0;i<10 && !atomic_load(&s_setup_request);i++) pump_keys(100);
    }
player_finished:
    release_player();
    s_connection_provider=NULL;
    s_connection_provider_ctx=NULL;
    ESP_LOGW(TAG,"TV leave heap=%u",(unsigned)esp_get_free_heap_size());
    atomic_store(&s_running,false);
    s_task=NULL;
    if(s_done) s_done();
    vTaskDelete(NULL);
}


// --- FoloTalk entry points -------------------------------------------------
void ft_tv_key(int key, int event) {
    if (!atomic_load(&s_running)) return;
    on_key((bsp_btn_t)key,(bsp_btn_ev_t)event,NULL);
}
void ft_tv_inspect(void) {
    if (atomic_load(&s_running)) on_key((bsp_btn_t)-1,BSP_BTN_CLICK,NULL);
}
bool ft_tv_running(void) { return atomic_load(&s_running); }
void ft_tv_stop(void) {
    if (!atomic_load(&s_running)) return;
    atomic_store(&s_setup_request,true);
    taskENTER_CRITICAL(&player_lock);
    EventGroupHandle_t events=s_player ? s_player->events : NULL;
    taskEXIT_CRITICAL(&player_lock);
    if (events) xEventGroupSetBits(events,STOP);
}
esp_err_t ft_tv_start(const ft_tv_config_t *c) {
    if (!c || !c->stripes ||
        c->stripes_bytes < FT_TV_STRIPE_BYTES || !c->control ||
        c->control_bytes < FT_TV_WORKSPACE_BYTES || !c->frame_extra ||
        c->frame_extra_bytes < 8192u || c->frame_extra_bytes > 24576u ||
        (!c->connection_provider && (!c->host || !c->host[0] || !c->ticket ||
            strnlen(c->ticket,sizeof(s_ticket)) >= sizeof(s_ticket) ||
            strnlen(c->host,sizeof(s_target.host)) >= sizeof(s_target.host))) ||
        (c->session_task_stack_bytes &&
            (c->session_task_stack_bytes < FT_TV_SESSION_STACK_DEFAULT ||
             c->session_task_stack_bytes > FT_TV_SESSION_STACK_MAX || c->session_task_stack_bytes%4u)))
        return ESP_ERR_INVALID_ARG;
    if (atomic_exchange(&s_running,true)) return ESP_ERR_INVALID_STATE;
    s_host_allocation_failed=c->allocation_failed;
    atomic_store(&heap_failures,0u);atomic_store(&heap_media_failures,0u);
    atomic_store(&heap_failed_bytes,0u);atomic_store(&heap_failed_caps,0u);
    atomic_store(&heap_media_phase,false);
    (void)heap_caps_register_failed_alloc_callback(tv_heap_failed);
    s_player=allocate_player();
    if (!s_player) { release_player();atomic_store(&s_running,false); return ESP_ERR_NO_MEM; }
    s.events=xEventGroupCreate();
    s.keys=xQueueCreate(8,sizeof(key_ev_t));
    if (!s.events || !s.keys) { release_player(); atomic_store(&s_running,false); return ESP_ERR_NO_MEM; }
    s_connection_provider=c->connection_provider;
    s_connection_provider_ctx=c->connection_provider_ctx;
    memset(&s_target,0,sizeof(s_target));
    memset(s_ticket,0,sizeof(s_ticket));
    if(!s_connection_provider) {
        snprintf(s_target.host,sizeof(s_target.host),"%s",c->host);
        s_target.port=c->port;
        snprintf(s_ticket,sizeof(s_ticket),"%s",c->ticket);
    }
    s_lvgl_stripes=c->stripes;
    s_control=c->control;
    s_frame_regions[0]=(av_frame_region_t){c->control,FT_TV_WORKSPACE_BYTES};
    s_frame_regions[1]=(av_frame_region_t){c->frame_extra,(uint32_t)c->frame_extra_bytes};
    s_frame_regions[2]=(av_frame_region_t){c->stripes+FT_TV_STRIPE_BYTES,
        (uint32_t)((c->stripes_bytes-FT_TV_STRIPE_BYTES)&~(size_t)3u)};
    s.volume=c->volume;
    s_brightness=c->brightness;
    s_done=c->done;
    s_settings=c->settings;
    s_prepare=c->prepare;
    atomic_store(&s_setup_request,false);
    // Session loop: allocates nothing large itself; sessions do, and free on exit.
    uint32_t stack_bytes=c->session_task_stack_bytes?c->session_task_stack_bytes:FT_TV_SESSION_STACK_DEFAULT;
    if (xTaskCreate(tv_task,"ft_tv",stack_bytes,NULL,4,&s_task)!=pdPASS) {
        release_player();
        atomic_store(&s_running,false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
