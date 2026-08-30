#include <stdint.h>
#include <stdio.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxspu.h>
#include "sx_format.h"
#include "audio_tx.h"
#include "ofdm_tx.h"
#ifdef SX_DUMP_MODEM
#include "ofdm.h"
#include "ofdm_mod.h"
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define CONTAINER_CAP (SX_BIOS_SIZE + 65536u)
#ifndef SX_TRANSFER_SIZE
#define SX_TRANSFER_SIZE SX_BIOS_SIZE /* full 512 KiB BIOS transfer */
#endif

static DISPENV disp[2];
static DRAWENV draw[2];
static uint8_t pad_data[2][34];
static uint8_t container[CONTAINER_CAP];
static int startup_page;
static sx_container_mode_t container_mode = SX_CONTAINER_MIXED;
#define BURST_AUDIO_MODE 0u /* Burst Edition is always normal stereo. */
static unsigned wire_block_kib = SX_WIRE_BLOCK_DEFAULT_BYTES / 1024u;
static TILE ui_tile[2][4];
static SPRT marker_sprite;
static DR_TPAGE marker_tpage;
static int status_font;
extern const uint8_t marker_tim[];

static void init_marker_sprite(void) {
    TIM_IMAGE tim;
    GetTimInfo((const uint32_t *)marker_tim, &tim);
    LoadImage(tim.crect, tim.caddr);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    setSprt(&marker_sprite);
    setShadeTex(&marker_sprite, 1);
    setXY0(&marker_sprite, 296, 32);
    setWH(&marker_sprite, 8, 8);
    setUV0(&marker_sprite, 0, 224);
    setClut(&marker_sprite, 256, 480);
    setRGB0(&marker_sprite, 128, 128, 128);
    setDrawTPage(&marker_tpage, 0, 0, getTPage(0, 0, 768, 256));
}

static void draw_ui(unsigned page, uint8_t brightness, unsigned progress, int transferring, int splash) {
    /* Select the target page before issuing any primitives. */
    PutDrawEnv(&draw[page]);
    (void)brightness;
    unsigned filled = progress > 1000u ? 1000u : progress;
    setTile(&ui_tile[page][0]); setXY0(&ui_tile[page][0], 0, 4); setWH(&ui_tile[page][0], 320, 16);
    setRGB0(&ui_tile[page][0], 70, 3, 12);
    setTile(&ui_tile[page][1]); setXY0(&ui_tile[page][1], 0, 4); setWH(&ui_tile[page][1], 8, 216);
    setRGB0(&ui_tile[page][1], transferring ? 255 : 150, transferring ? 48 : 12, 18);
    setTile(&ui_tile[page][2]); setXY0(&ui_tile[page][2], 16, 211); setWH(&ui_tile[page][2], 288, 12);
    setRGB0(&ui_tile[page][2], 12, 4, 8);
    setTile(&ui_tile[page][3]); setXY0(&ui_tile[page][3], 16, 211);
    setWH(&ui_tile[page][3], (int)(288u * filled / 1000u), 12);
    setRGB0(&ui_tile[page][3], splash ? 220 : 255, splash ? 24 : 58, splash ? 45 : 18);
    DrawPrim((const uint32_t *)&ui_tile[page][0]);
    DrawPrim((const uint32_t *)&ui_tile[page][1]);
    DrawPrim((const uint32_t *)&ui_tile[page][2]);
    if (filled) DrawPrim((const uint32_t *)&ui_tile[page][3]);
    if (transferring && ((VSync(-1) / 6) & 1)) {
    }
}

static void init(void) {
    InitGeom();
    ResetGraph(0);
    for (int i = 0; i < 2; i++) {
        SetDefDispEnv(&disp[i], 0, 0, SCREEN_W, SCREEN_H);
        SetDefDrawEnv(&draw[i], 0, 0, SCREEN_W, SCREEN_H);
        disp[i].isinter = 0; draw[i].dtd = 1;
        setRGB0(&draw[i], 26, 4, 8); draw[i].isbg = 1;
    }
    FntLoad(960, 0);
    status_font = FntOpen(18, 25, 284, 180, 0, 512);
    SpuInit();
    init_marker_sprite();
    InitPAD(pad_data[0], 34, pad_data[1], 34); StartPAD(); ChangeClearPAD(0);
    SetDispMask(1);
}

static void compression_progress(uint16_t completed, uint16_t total, size_t stored, void *user) {
    (void)user;
    unsigned permille = total ? completed * 1000u / total : 0;
    draw_ui(startup_page, 128, permille, 0, 1);
    FntPrint(status_font, "PSX-BASIC(SX) Ver 1.0\n\n");
    FntPrint(status_font, "PS1 BIOS AUDIO RIPPER SX\n\n");
    FntPrint(status_font, "LZSS COMPRESSING...\n\n");
    FntPrint(status_font, "BLOCK %03u/%03u\n", completed, total);
    FntPrint(status_font, "PROGRESS %3u.%u%%\n", permille / 10u, permille % 10u);
    FntPrint(status_font, "STORED %u BYTES\n", (unsigned)stored);
    FntFlush(status_font); DrawSync(0); VSync(0); PutDispEnv(&disp[startup_page]);
}

static void compression_done(void) {
    draw_ui(startup_page, 128, 1000, 0, 1);
    FntPrint(status_font, "PSX-BASIC(SX) Ver 1.0\n\n");
    FntPrint(status_font, "LZSS COMPLETE\n\nOk\n");
    FntFlush(status_font); DrawSync(0); VSync(0); PutDispEnv(&disp[startup_page]);
}

static void switch_to_transfer_video(void) {
    for (int i = 0; i < 2; i++) {
        /* Keep one VRAM page: the SDK font stream uses fixed absolute
         * coordinates and must not move to an alternate Y page. */
        SetDefDispEnv(&disp[i], 0, 0, 320, 240);
        SetDefDrawEnv(&draw[i], 0, 0, 320, 240);
        draw[i].dtd = 1;
        setRGB0(&draw[i], 26, 4, 8); draw[i].isbg = 1;
    }
    PutDispEnv(&disp[0]); PutDrawEnv(&draw[0]);
    status_font = FntOpen(18, 25, 284, 180, 0, 512);
}

int main(void) {
    init();
#ifdef SX_DUMP_MODEM
    {
        static uint8_t src[20000], pkt[SX_OFDM_PACKET_BYTES];
        static int16_t L[SX_OFDM_PACKET_SAMPLES], R[SX_OFDM_PACKET_SAMPLES];
        for (unsigned i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 29u + 7u);
        sx_ofdm_make_packet(src, sizeof(src), sx_crc32(src, sizeof(src), 0), 3, pkt);
        sx_ofdm_modulate_packet(pkt, 3, L, R);
        uint32_t ck = 0; int32_t pL = 0, pR = 0;
        for (unsigned i = 0; i < SX_OFDM_PACKET_SAMPLES; i++) {
            ck = ck * 1000003u + (uint16_t)L[i]; ck = ck * 1000003u + (uint16_t)R[i];
            int32_t al = L[i] < 0 ? -L[i] : L[i], ar = R[i] < 0 ? -R[i] : R[i];
            if (al > pL) pL = al; if (ar > pR) pR = ar;
        }
        uint32_t ckR = 0;
        for (unsigned i = 0; i < SX_OFDM_PACKET_SAMPLES; i++) ckR = ckR * 1000003u + (uint16_t)R[i];
        int c0 = (int)hicos(0), c90 = (int)hicos(1048576), s90 = (int)hisin(1048576);
        static volatile uint32_t probe[24];
        probe[0] = 0x5A50444Du; probe[1] = 0x30545345u;
        probe[2] = ck; probe[3] = ckR; probe[4] = (uint32_t)pL; probe[5] = (uint32_t)pR;
        for (unsigned i = 0; i < 16; i++) probe[6 + i] = (uint32_t)(int32_t)L[i];
        int p = 0;
        for (;;) {
            FntPrint(-1, "PS1 MODEM SELFTEST pkt3 n=%u\n\n", SX_OFDM_PACKET_SAMPLES);
            FntPrint(-1, "CK L %08x  R %08x\n", ck, ckR);
            FntPrint(-1, "PEAK L %d R %d\n", (int)pL, (int)pR);
            FntPrint(-1, "L[0..7] %d %d %d %d\n        %d %d %d %d\n",
                L[0], L[1], L[2], L[3], L[4], L[5], L[6], L[7]);
            FntPrint(-1, "hicos0=%d hicos90=%d hisin90=%d\n", c0, c90, s90);
            FntPrint(-1, "\nHOST: CK L 998515a0  L0..3 0 -288 -416 -64\nhicos0=4096 hicos90=0 hisin90=4096\n");
            DrawSync(0); VSync(0);
            PutDispEnv(&disp[p]); PutDrawEnv(&draw[p]);
            FntFlush(-1); p ^= 1;
        }
    }
#endif
    const uint8_t *bios = (const uint8_t *)SX_BIOS_ADDR;
    draw_ui(startup_page, 128, 0, 0, 1);
    FntPrint(status_font, "PSX-BASIC(SX) Ver 1.0\n\n");
    FntPrint(status_font, "PS1 BIOS AUDIO RIPPER SX\n\n");
    FntPrint(status_font, "LZSS PREPARING...\n");
    FntFlush(status_font); DrawSync(0); VSync(0); PutDispEnv(&disp[startup_page]);
    uint32_t crc = sx_crc32(bios, SX_BIOS_SIZE, 0);
    size_t packed = sx_build_container_progress_mode(bios, SX_TRANSFER_SIZE, container, sizeof(container),
                                                     compression_progress, 0, container_mode);
    compression_done();
    switch_to_transfer_video();
    unsigned lzss_blocks = 0, raw_blocks = 0;
    if (packed >= sizeof(sx_header_t)) {
        const sx_header_t *header = (const sx_header_t *)container;
        size_t offset = sizeof(*header);
        for (unsigned i = 0; i < header->block_count && offset + sizeof(sx_block_header_t) <= packed; i++) {
            const sx_block_header_t *block = (const sx_block_header_t *)(container + offset);
            if (block->codec == SX_CODEC_LZSS) lzss_blocks++; else raw_blocks++;
            offset += sizeof(*block) + block->stored_size;
        }
    }
    uint16_t previous = 0xffff;
    int page = 0;
    int command_issued = 0;
    int clear_text_frames = 0;
    int stopped = 0;
    unsigned boot_line = 0;
    unsigned boot_char = 0;
    int boot_next_frame = 0;
    static const char load_command[] = "LOAD\"BURST\"";
    static const char run_command[] = "RUN";
#ifdef SX_TEST_AUTOSTART
    int autostarted = 0;
#endif
    for (;;) {
        sx_audio_update();
        const PADTYPE *pad = (const PADTYPE *)pad_data[0];
        uint16_t buttons = pad->stat ? 0xffff : pad->btn;
#ifdef SX_TEST_AUTOSTART
        if (!autostarted && packed) { autostarted = 1; sx_audio_transmit(container, packed); }
#endif
        const sx_tx_status_t *current_tx = sx_audio_status();
        sx_audio_set_mode(BURST_AUDIO_MODE);
        sx_audio_set_block_bytes(wire_block_kib * 1024u);
        sx_ofdm_tx_set_audio_mode(BURST_AUDIO_MODE);
        if ((previous & PAD_CROSS) && !(buttons & PAD_CROSS)) {
            sx_audio_stop();
            stopped = 1;
        }
        int tx_inactive = current_tx->phase == SX_TX_IDLE || current_tx->phase == SX_TX_DONE ||
                          current_tx->phase == SX_TX_ERROR;
        if (tx_inactive && (((previous & PAD_START) && !(buttons & PAD_START)) ||
             ((previous & PAD_CIRCLE) && !(buttons & PAD_CIRCLE))) && packed) {
            command_issued = 1;
            clear_text_frames = 2;
            stopped = 0;
            sx_audio_transmit(container, packed);
        }
        previous = buttons;
        const sx_tx_status_t *tx = sx_audio_status();
        if (!command_issued) {
            int frame = VSync(-1);
            if (frame >= boot_next_frame) {
                if (boot_line < 4) {
                    boot_line++;
                    boot_next_frame = frame + 8;
                } else if (boot_line == 4) {
                    boot_line = 5;
                    boot_char = 0;
                    boot_next_frame = frame + 3;
                } else if (boot_line == 5) {
                    if (boot_char < sizeof(load_command) - 1) {
                        boot_char++;
                        boot_next_frame = frame + 3;
                    } else {
                        boot_line = 6;
                        boot_next_frame = frame + 8;
                    }
                } else if (boot_line == 6) {
                    boot_line = 7;
                    boot_char = 0;
                    boot_next_frame = frame + 3;
                } else if (boot_line == 7) {
                    if (boot_char < sizeof(run_command) - 1) {
                        boot_char++;
                        boot_next_frame = frame + 3;
                    } else {
                        boot_line = 8;
                        boot_next_frame = frame + 8;
                    }
                } else if (boot_line == 8) {
                    boot_line = 9;
                    boot_next_frame = frame + 8;
                }
            }
        }
        /* Do not spend another video frame drawing while the real-time audio
         * FIFO needs data. SPU IRQ/DMA continues independently. */
        if (tx->phase == SX_TX_SENDING || tx->phase == SX_TX_PREPARING) {
            const sx_ofdm_tx_status_t *ofdm = sx_ofdm_tx_status();
            if (ofdm->fifo_packets < ofdm->fifo_capacity) continue;
        }
        int transferring = tx->phase == SX_TX_PREPARING || tx->phase == SX_TX_SENDING;
        unsigned transfer_progress = tx->progress_permille;
        if (tx->phase == SX_TX_DONE) transfer_progress = 1000;
        draw_ui(page, transferring ? 128 : 96, transfer_progress, transferring, 0);
        if (clear_text_frames) {
            clear_text_frames--;
            goto render_frame;
        }
        if (tx->phase == SX_TX_ERROR) {
            const sx_ofdm_tx_status_t *ofdm = sx_ofdm_tx_status();
            FntPrint(status_font, "PS1 BIOS RIPPER SX\nBURST EDITION / WIRE V6\n\n");
            if (tx->error == -2) FntPrint(status_font, "OFDM FIFO UNDERRUN  E-2\n\n");
            else if (tx->error == -1) FntPrint(status_font, "OFDM PACKET ERROR  E-1\n\n");
            else if (tx->error == -3) FntPrint(status_font, "OFDM START ERROR  E-3\n\n");
            else FntPrint(status_font, "AUDIO TX ERROR %d\n\n", tx->error);
            FntPrint(status_font, "PACKET %u FRAME\nMODEM  %u FRAME\nADPCM  %u FRAME\n",
                ofdm->packet_frames, ofdm->modem_frames, ofdm->adpcm_frames);
            FntPrint(status_font, "TOTAL  %u / MAX %u FRAME\n\n",
                ofdm->last_generate_frames, ofdm->max_generate_frames);
            FntPrint(status_font, "GENERATED %u / PLAYED %u / %u\nFIFO %u / %u\n",
                ofdm->generated_packets, ofdm->played_packet, ofdm->total_packets,
                ofdm->fifo_packets, ofdm->fifo_capacity);
            goto render_frame;
        }
        if (!command_issued) {
            if (boot_line >= 1) FntPrint(status_font, "PS1 BIOS RIPPER SX\n");
            if (boot_line >= 2) FntPrint(status_font, "2MB RAM SYSTEM\n");
            if (boot_line >= 3) FntPrint(status_font, "1MB VRAM 512KB SPU RAM\n\n");
            if (boot_line >= 4) FntPrint(status_font, "BURST EDITION / WIRE V6\n");
            if (boot_line >= 5) {
                unsigned load_chars = boot_line == 5 ? boot_char : sizeof(load_command) - 1u;
                for (unsigned i = 0; i < load_chars; i++)
                    FntPrint(status_font, "%c", load_command[i]);
                FntPrint(status_font, "\n");
            }
            if (boot_line >= 6) FntPrint(status_font, "Ok\n\n");
            if (boot_line >= 7) {
                unsigned run_chars = boot_line == 7 ? boot_char : sizeof(run_command) - 1u;
                for (unsigned i = 0; i < run_chars; i++)
                    FntPrint(status_font, "%c", run_command[i]);
                FntPrint(status_font, "\n");
            }
            if (boot_line >= 8) FntPrint(status_font, "Ok\n\n");
            if (boot_line >= 9) {
                FntPrint(status_font, "MODE: OFDM STEREO\n");
                FntPrint(status_font, "BLOCK SIZE: %u KiB\n", wire_block_kib);
                FntPrint(status_font, "FEC: 32+4 / CONTINUOUS STREAM\n");
                if ((VSync(-1) / 30) & 1)
                    FntPrint(status_font, "PRESS START BUTTON\n");
            }
            goto render_frame;
        }
        FntPrint(status_font, "PS1 BIOS RIPPER SX // BURST\nRACE CONTROL / WIRE V6\n----------------------------\n");
        if (transferring)
            FntPrint(status_font, "LAP %03u/%03u\n", tx->block + 1u, tx->blocks);
        FntPrint(status_font, "LOAD\"BURST\"\nOk\nRUN\nOk\n");
        if (!command_issued) {
            if ((VSync(-1) / 6) & 1) FntPrint(status_font, "PRESS START BUTTON\n");
        } else {
            FntPrint(status_font, "CSAVE \"PS1-BIOS\"\n");
            FntPrint(status_font, "BIOS %u BYTES\nCRC32 %08x\n", SX_TRANSFER_SIZE, crc);
            FntPrint(status_font, "SX %u bytes / %u blocks\n", (unsigned)packed,
                     (unsigned)((SX_TRANSFER_SIZE + SX_BLOCK_SIZE - 1) / SX_BLOCK_SIZE));
            FntPrint(status_font, "CODEC LZSS:%u RAW:%u %u%%\n", lzss_blocks, raw_blocks,
                     packed ? (unsigned)((packed * 100u) / SX_BIOS_SIZE) : 0);
            FntPrint(status_font, "AUDIO [OFDM STEREO]\n");
            FntPrint(status_font, "BLOCK SIZE %u KiB / FEC 32+4\n", wire_block_kib);
            FntPrint(status_font, "CONTINUOUS OFDM / NO FSK\n");
        }
        if (command_issued && tx->phase == SX_TX_PREPARING) {
            FntPrint(status_font, "[ GRID ] OFDM DATA PREPARE\n");
            const sx_ofdm_tx_status_t *ofdm = sx_ofdm_tx_status();
            FntPrint(status_font, "BLOCK %03u/%03u  CRC32 %08x\n",
                tx->block + 1u, tx->blocks, tx->block_crc);
            FntPrint(status_font, "OFDM BUFFER %u/%u PKT %u/%u\n",
                ofdm->fifo_packets, ofdm->fifo_capacity, ofdm->generated_packets, ofdm->total_packets);
            FntPrint(status_font, "GEN %uF MAX %uF\n", ofdm->last_generate_frames, ofdm->max_generate_frames);
        } else if (command_issued && tx->phase == SX_TX_SENDING) {
            FntPrint(status_font, "[ RACING ] OFDM DATA SEND\n");
            const sx_ofdm_tx_status_t *ofdm = sx_ofdm_tx_status();
            FntPrint(status_font, "BLOCK %03u/%03u  CRC32 %08x\n",
                tx->block + 1u, tx->blocks, tx->block_crc);
            FntPrint(status_font, "LAP %03u/%03u  TOTAL %u.%u%%\nOFDM PKT %u/%u  FIFO %u/%u\nGEN P%u M%u A%u  T%u MAX%u\n",
                tx->block + 1u, tx->blocks,
                tx->progress_permille/10, tx->progress_permille%10,
                ofdm->played_packet, ofdm->total_packets,
                ofdm->fifo_packets, ofdm->fifo_capacity,
                ofdm->packet_frames, ofdm->modem_frames, ofdm->adpcm_frames,
                ofdm->last_generate_frames, ofdm->max_generate_frames);
        } else if (command_issued && tx->phase == SX_TX_DONE) {
            FntPrint(status_font, "BLOCKS %03u/%03u  %u BYTES EACH\n",
                tx->blocks, tx->blocks, tx->block_bytes);
            FntPrint(status_font, "OFDM PAYLOAD SENT 100.0%%\nTRANSFER COMPLETE\nOk\n");
        } else if (command_issued && stopped) {
            FntPrint(status_font, "AUDIO OUTPUT STOPPED\n\n");
            FntPrint(status_font, "CIRCLE/START: TRANSMIT  CROSS: STOP\nOk\n");
        }
        else if (command_issued) FntPrint(status_font, "CONNECT AUDIO L/R TO PC\nCIRCLE/START: TRANSMIT  CROSS: STOP\n");
render_frame:
        if (transferring && ((VSync(-1) / 6) & 1)) {
            DrawPrim((const uint32_t *)&marker_tpage);
            DrawPrim((const uint32_t *)&marker_sprite);
        }
        FntFlush(status_font);
        DrawSync(0); VSync(0); PutDispEnv(&disp[page]); page ^= 1;
    }
}
