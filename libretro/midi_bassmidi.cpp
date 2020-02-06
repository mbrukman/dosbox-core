// This is copyrighted software. More information is at the end of this file.
#include "midi_bassmidi.h"

#include "control.h"
#include "libretro_dosbox.h"
#include <dlfcn.h>
#include <tuple>

/* We don't link against the BASS/BASSMIDI libraries. We dlopen() them so that the core can be
 * distributed without those libs in a GPL-compliant way.
 */
#define BASS_CONFIG_MIDI_DEFFONT 0x10403
#define BASS_STREAM_DECODE 0x200000
#define BASS_MIDI_SINCINTER 0x800000
#define BASS_MIDI_EVENTS_RAW 0x10000
#define BASS_MIDI_EVENTS_NORSTATUS 0x2000000

using HSTREAM = MidiHandlerBassmidi::HSTREAM;

extern "C" {
static auto (*BASS_ChannelGetData)(uint32_t handle, void* buffer, uint32_t length) -> uint32_t;
static auto (*BASS_ErrorGetCode)() -> int;
static auto (*BASS_Init)(int device, uint32_t freq, uint32_t flags, void* win, void* dsguid) -> int;
static auto (*BASS_SetConfigPtr)(uint32_t option, const void* value) -> int;
static auto (*BASS_StreamFree)(HSTREAM handle) -> int;

static auto (*BASS_MIDI_StreamCreate)(uint32_t channels, uint32_t flags, uint32_t freq) -> HSTREAM;
static auto (*BASS_MIDI_StreamEvents)(
    HSTREAM handle, uint32_t mode, const void* events, uint32_t length) -> uint32_t;
}

MidiHandlerBassmidi MidiHandlerBassmidi::instance_;
bool MidiHandlerBassmidi::bass_initialized_ = false;
bool MidiHandlerBassmidi::bass_libs_loaded_ = false;
MidiHandlerBassmidi::Dlhandle_t MidiHandlerBassmidi::bass_lib_{nullptr, dlcloseWrapper};
MidiHandlerBassmidi::Dlhandle_t MidiHandlerBassmidi::bassmidi_lib_{nullptr, dlcloseWrapper};

MidiHandlerBassmidi::~MidiHandlerBassmidi()
{
    Close();
}

void MidiHandlerBassmidi::initDosboxSettings()
{
    auto init_func = [](Section* const) {
        if (instance_.is_open_) {
            instance_.Open(nullptr);
        }
    };
    auto* secprop = control->AddSection_prop("bassmidi", init_func, true);
    secprop->AddDestroyFunction([](Section* const) { instance_.Close(); });
    auto* str_prop = secprop->Add_string("bassmidi.soundfont", Property::Changeable::WhenIdle, "");
    str_prop->Set_help("Soundfont to use with BASSMIDI. One must be specified.");
}

auto MidiHandlerBassmidi::Open(const char* const /*conf*/) -> bool
{
    if (!bass_libs_loaded_) {
        if (const auto [ok, msg] = loadLibs(); !ok) {
            log_cb(RETRO_LOG_WARN, "[dosbox] failed to load BASS libraries: %s\n", msg.c_str());
            return false;
        }
    }

    Close();

    if (!bass_initialized_ && !BASS_Init(0, 44100, 0, nullptr, nullptr)) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] bassmidi: failed to initialize BASS: code %d\n",
            BASS_ErrorGetCode());
        return false;
    }
    bass_initialized_ = true;

    auto* section = static_cast<Section_prop*>(control->GetSection("bassmidi"));

    std::string_view soundfont = section->Get_string("bassmidi.soundfont");
    if (!soundfont.empty() && !BASS_SetConfigPtr(BASS_CONFIG_MIDI_DEFFONT, soundfont.data())) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] bassmidi: failed to set soundfont: code %d\n",
            BASS_ErrorGetCode());
    }

    stream_ = BASS_MIDI_StreamCreate(16, BASS_STREAM_DECODE | BASS_MIDI_SINCINTER, 0);
    if (stream_ == 0) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] failed to create BASSMIDI stream: code %d\n",
            BASS_ErrorGetCode());
        return false;
    }

    MixerChannel_ptr_t channel(MIXER_AddChannel(mixerCallback, 44100, "BASSMID"), MIXER_DelChannel);
    channel->Enable(true);

    channel_ = std::move(channel);
    is_open_ = true;
    return true;
}

void MidiHandlerBassmidi::Close()
{
    if (!is_open_) {
        return;
    }

    channel_->Enable(false);
    channel_ = nullptr;
    BASS_StreamFree(stream_);
    stream_ = 0;
    is_open_ = false;
}

void MidiHandlerBassmidi::PlayMsg(Bit8u* const msg)
{
    int msg_len = sizeof(DB_Midi::rt_buf);

    switch ((msg[0] & 0b1111'0000) >> 4) {
    case 0b1000:
    case 0b1001:
    case 0b1010:
    case 0b1011:
    case 0b1110:
        msg_len = 3;
        break;

    case 0b1100:
    case 0b1101:
        msg_len = 2;
        break;
    }

    if (BASS_MIDI_StreamEvents(
            stream_, BASS_MIDI_EVENTS_RAW | BASS_MIDI_EVENTS_NORSTATUS, msg, msg_len)
        != 1)
    {
        uint64_t tmp = 0;
        static_assert(sizeof(tmp) == sizeof(DB_Midi::rt_buf));
        static_assert(sizeof(tmp) == sizeof(DB_Midi::cmd_buf));
        memcpy(&tmp, msg, msg_len);
        if (msg_len == sizeof(tmp)) {
            log_cb(
                RETRO_LOG_WARN, "[dosbox] bassmidi: unknown MIDI message %08lx: code %d\n", tmp,
                BASS_ErrorGetCode());
        } else {
            log_cb(
                RETRO_LOG_WARN, "[dosbox] bassmidi: error playing MIDI message %08lx: code %d\n",
                tmp, BASS_ErrorGetCode());
        }
    }
}

void MidiHandlerBassmidi::PlaySysex(Bit8u* const sysex, const Bitu len)
{
    if (BASS_MIDI_StreamEvents(stream_, BASS_MIDI_EVENTS_RAW, sysex, len) == -1u) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] bassmidi: error playing MIDI sysex: code %d\n",
            BASS_ErrorGetCode());
    }
}

auto MidiHandlerBassmidi::GetName() -> const char*
{
    return "bassmidi";
}

void MidiHandlerBassmidi::mixerCallback(const Bitu len)
{
    if (BASS_ChannelGetData(instance_.stream_, MixTemp, len * 4) == -1u) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] bassmidi: error rendering audio: code %d\n",
            BASS_ErrorGetCode());
    }
    instance_.channel_->AddSamples_s16(len, reinterpret_cast<Bit16s*>(MixTemp));
}

void MidiHandlerBassmidi::dlcloseWrapper(void* handle)
{
    if (handle) {
        dlclose(handle);
    }
}

auto MidiHandlerBassmidi::loadLibs() -> std::tuple<bool, std::string>
{
#ifdef WIN32
    constexpr const char* bass_name = "bass.dll";
    constexpr const char* bassmidi_name = "bassmidi.dll";
#elif defined(__MACOSX__)
    constexpr const char* bass_name = "libbass.dylib";
    constexpr const char* bassmidi_name = "libbassmidi.dylib";
#else
    constexpr const char* bass_name = "libbass.so";
    constexpr const char* bassmidi_name = "libbassmidi.so";
#endif

    dlerror();

    Dlhandle_t basslib(
        dlopen((retro_system_directory / bass_name).u8string().c_str(), RTLD_NOW | RTLD_GLOBAL),
        dlcloseWrapper);
    if (!basslib) {
        return {false, dlerror()};
    }
    Dlhandle_t midilib(
        dlopen((retro_system_directory / bassmidi_name).u8string().c_str(), RTLD_NOW | RTLD_GLOBAL),
        dlcloseWrapper);
    if (!midilib) {
        return {false, dlerror()};
    }

    if (!(BASS_ChannelGetData =
              (decltype(BASS_ChannelGetData))dlsym(basslib.get(), "BASS_ChannelGetData")))
    {
        return {false, dlerror()};
    }
    if (!(BASS_ErrorGetCode =
              (decltype(BASS_ErrorGetCode))dlsym(basslib.get(), "BASS_ErrorGetCode"))) {
        return {false, dlerror()};
    }
    if (!(BASS_Init = (decltype(BASS_Init))dlsym(basslib.get(), "BASS_Init"))) {
        return {false, dlerror()};
    }
    if (!(BASS_SetConfigPtr =
              (decltype(BASS_SetConfigPtr))dlsym(basslib.get(), "BASS_SetConfigPtr"))) {
        return {false, dlerror()};
    }
    if (!(BASS_StreamFree = (decltype(BASS_StreamFree))dlsym(basslib.get(), "BASS_StreamFree"))) {
        return {false, dlerror()};
    }
    if (!(BASS_MIDI_StreamCreate =
              (decltype(BASS_MIDI_StreamCreate))dlsym(midilib.get(), "BASS_MIDI_StreamCreate")))
    {
        return {false, dlerror()};
    }
    if (!(BASS_MIDI_StreamEvents =
              (decltype(BASS_MIDI_StreamEvents))dlsym(midilib.get(), "BASS_MIDI_StreamEvents")))
    {
        return {false, dlerror()};
    }

    bass_lib_ = std::move(basslib);
    bassmidi_lib_ = std::move(midilib);
    bass_libs_loaded_ = true;
    return {true, ""};
}

/*

Copyright (C) 2002-2011 The DOSBox Team
Copyright (C) 2020 Nikos Chantziaras <realnc@gmail.com>

This file is part of DOSBox-core.

DOSBox-core is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version.

DOSBox-core is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
DOSBox-core. If not, see <https://www.gnu.org/licenses/>.

*/
