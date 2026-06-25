/*
 * Tab5-Meshtastic v2 — UI design tokens.
 *
 * The dark design system carried forward from the established Tab5-Meshtastic
 * design (PRD §8). 1280x720 landscape. Keep all palette/type/metric constants
 * here so screens stay consistent and the system is tweakable in one place.
 */
#pragma once

/* ---- Palette (hex 0xRRGGBB) ---- */
#define C_BG       0x0C0F0E  /* base background                         */
#define C_CHROME   0x0A0D0C  /* rail / status bar / composer            */
#define C_SURF     0x1C2320  /* surface / key / off-bar / border        */
#define C_SURF2    0x161C19  /* surface alt (cards / inputs)            */
#define C_PANEL    0x0F1412  /* overlay panel                           */
#define C_GREEN    0x58D98A  /* accent green                            */
#define C_INK      0x07120C  /* green ink (text on green)               */
#define C_AMBER    0xF2C14E  /* warn                                    */
#define C_RED      0xEC6A5A  /* error                                   */
#define C_HI       0xE9EFEC  /* text high                               */
#define C_MID      0x93A09A  /* text mid                                */
#define C_DIM      0x5E6A64  /* text dim                                */
#define C_HAIRLINE 0x2C352F  /* hairline                                */

/* ---- Type scale (Montserrat; weights 600-800 on device) ---- */
#define FONT_TITLE &lv_font_montserrat_28  /* 26 title  */
#define FONT_BADGE &lv_font_montserrat_22  /* badge     */
#define FONT_ROW   &lv_font_montserrat_18  /* row/body  */
#define FONT_BODY  &lv_font_montserrat_16  /* message   */
#define FONT_META  &lv_font_montserrat_14  /* 13 meta   */

/* ---- Metrics (8px grid) ---- */
#define M_RAIL_W   88   /* side rail width            */
#define M_RAIL_BTN 64   /* rail button                */
#define M_STATUS_H 56   /* status bar height          */
#define M_ROW_H    58   /* list row height            */
#define M_TOUCH    48   /* minimum touch target       */
#define M_RAD_S    9    /* small radius               */
#define M_RAD_M    12   /* medium radius              */
#define M_RAD_L    14   /* large radius               */
#define M_RAD_PILL 7    /* pill radius                */
