/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
 /*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TEXT_DESCRIPTIONS_H_

#define TEXT_DESCRIPTIONS_H_

#include <binder/Parcel.h>
#include <media/stagefright/foundation/ABase.h>

namespace android {

class TextDescriptions {
public:
    enum {
        IN_BAND_TEXT_3GPP             = 0x01,
        OUT_OF_BAND_TEXT_SRT          = 0x02,

#ifdef MTK_SUBTITLE_SUPPORT
        IN_BAND_TEXT_VOBSUB             = 0x03,
        IN_BAND_TEXT_ASS             = 0x04,
        IN_BAND_TEXT_SSA             = 0x05,
        IN_BAND_TEXT_TXT             = 0x06,
        IN_BAND_TEXT_DVB		 	 = 0x07,
        OUT_OF_BAND_TEXT_ASS          = 0x08,
        OUT_OF_BAND_TEXT_SSA          = 0x09,
        OUT_OF_BAND_TEXT_TXT          = 0x0A,
        OUT_OF_BAND_TEXT_MPL          = 0x0B,
        OUT_OF_BAND_TEXT_SMI          = 0x0C,
        OUT_OF_BAND_TEXT_SUB          = 0x0D,
        OUT_OF_BAND_TEXT_IDX          = 0x0E,
#endif
        GLOBAL_DESCRIPTIONS           = 0x100,
        LOCAL_DESCRIPTIONS            = 0x200,
    };

    static status_t getParcelOfDescriptions(
            const uint8_t *data, ssize_t size,
            uint32_t flags, int timeMs, Parcel *parcel);
#ifdef MTK_SUBTITLE_SUPPORT
    static status_t getParcelOfDescriptions(
        int32_t fd, ssize_t width, ssize_t height,
        uint32_t flags, int timeMs, Parcel *parcel);
#endif
private:
    TextDescriptions();

    enum {
        // These keys must be in sync with the keys in TimedText.java
        KEY_DISPLAY_FLAGS                 = 1, // int
        KEY_STYLE_FLAGS                   = 2, // int
        KEY_BACKGROUND_COLOR_RGBA         = 3, // int
        KEY_HIGHLIGHT_COLOR_RGBA          = 4, // int
        KEY_SCROLL_DELAY                  = 5, // int
        KEY_WRAP_TEXT                     = 6, // int
        KEY_START_TIME                    = 7, // int
        KEY_STRUCT_BLINKING_TEXT_LIST     = 8, // List<CharPos>
        KEY_STRUCT_FONT_LIST              = 9, // List<Font>
        KEY_STRUCT_HIGHLIGHT_LIST         = 10, // List<CharPos>
        KEY_STRUCT_HYPER_TEXT_LIST        = 11, // List<HyperText>
        KEY_STRUCT_KARAOKE_LIST           = 12, // List<Karaoke>
        KEY_STRUCT_STYLE_LIST             = 13, // List<Style>
        KEY_STRUCT_TEXT_POS               = 14, // TextPos
        KEY_STRUCT_JUSTIFICATION          = 15, // Justification
        KEY_STRUCT_TEXT                   = 16, // Text
        KEY_STRUCT_BITMAP                 = 17, // Bitmap

        KEY_GLOBAL_SETTING                = 101,
        KEY_LOCAL_SETTING                 = 102,
        KEY_START_CHAR                    = 103,
        KEY_END_CHAR                      = 104,
        KEY_FONT_ID                       = 105,
        KEY_FONT_SIZE                     = 106,
        KEY_TEXT_COLOR_RGBA               = 107,
    };

    static status_t extractSRTLocalDescriptions(
            const uint8_t *data, ssize_t size,
            int timeMs, Parcel *parcel);
    static status_t extract3GPPGlobalDescriptions(
            const uint8_t *data, ssize_t size,
            Parcel *parcel, int depth);
    static status_t extract3GPPLocalDescriptions(
            const uint8_t *data, ssize_t size,
            int timeMs, Parcel *parcel, int depth);

#ifdef MTK_SUBTITLE_SUPPORT
    static status_t extractASSGlobalDescriptions(
        const uint8_t *data, ssize_t size, Parcel *parcel, int depth) ;

    static status_t extractASSLocalDescriptions(
        const uint8_t *data, ssize_t size,
        int timeMs, Parcel *parcel, int depth);
    static status_t extractSSAGlobalDescriptions(
        const uint8_t *data, ssize_t size, Parcel *parcel, int depth) ;

    static status_t extractSSALocalDescriptions(
        const uint8_t *data, ssize_t size,
        int timeMs, Parcel *parcel, int depth);
    static status_t extractTXTGlobalDescriptions(
        const uint8_t *data, ssize_t size, Parcel *parcel, int depth) ;

    static status_t extractTXTLocalDescriptions(
        const uint8_t *data, ssize_t size,
        int timeMs, Parcel *parcel, int depth);
    static status_t extractVOBSUBLocalDescriptions(
        int32_t fd, ssize_t width, ssize_t height, int timeMs, Parcel *parcel);
    static status_t extractDVBBLocalDescriptions(
        int32_t fd, ssize_t width, ssize_t height, int timeMs, Parcel *parcel);
#endif
    DISALLOW_EVIL_CONSTRUCTORS(TextDescriptions);
};

}  // namespace android
#endif  // TEXT_DESCRIPTIONS_H_
