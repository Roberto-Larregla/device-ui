#if defined(TFIELD_UI) && (HAS_TFT || defined(ARCH_PORTDUINO))

#include "graphics/view/TFT/TFieldView.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/PMTileService.h"
#include "input/InputDriver.h"
#include "lvgl.h"
#include "mesh-pb-constants.h"
#include "misc/cache/lv_cache.h" // lv_image_cache_drop() is not exported by lvgl.h in 9.6
#include "storage/IStorage.h"
#include "storage/MapStorage.h"
#include "util/ILog.h"
#include "util/PNGDecoder.h"

#include "tfield/app.h"
#include "tfield/format.h"
#include "tfield/i18n.h"
#include "tfield/settings.h"
#include "tfield/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <set>

#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
#include <random>
#else
#include "LittleFS.h"
#include "esp_random.h"
#endif
#if defined(T_DECK) && !defined(ARCH_PORTDUINO)
#include <Wire.h>
#endif

extern fs::FS &persistentFS; // ViewController.cpp: LittleFS in the device, a folder on Linux

// The firmware keeps its latest own GPS fix here. device-ui is linked into the firmware,
// so the track can follow the GPS every few seconds instead of waiting for a position
// broadcast. Weak: a build without the firmware (the native tests) simply has none.
extern meshtastic_Position localPosition __attribute__((weak));

using namespace tfield;

namespace
{
constexpr uint32_t BROADCAST = 0xffffffff;
constexpr const char *SETTINGS_FILE = "/tfield/ajustes.txt";
constexpr const char *QUICK_FILE = "/tfield/respuestas.txt";
constexpr const char *GPS_FILE = "/tfield/gps.txt";
// private-channel invitation and "I joined" (T-Field UI to T-Field UI, end-to-end encrypted)
constexpr uint8_t MAGIC[3] = {'T', 'F', '1'};
constexpr uint8_t PKT_INVITE = 'I', PKT_JOINED = 'J';

struct Code {
    int value;
    const char *code;
};
const Code REGIONS[] = {
    {meshtastic_Config_LoRaConfig_RegionCode_US, "US"},         {meshtastic_Config_LoRaConfig_RegionCode_EU_433, "EU_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_868, "EU_868"}, {meshtastic_Config_LoRaConfig_RegionCode_CN, "CN"},
    {meshtastic_Config_LoRaConfig_RegionCode_JP, "JP"},         {meshtastic_Config_LoRaConfig_RegionCode_ANZ, "ANZ"},
    {meshtastic_Config_LoRaConfig_RegionCode_KR, "KR"},         {meshtastic_Config_LoRaConfig_RegionCode_TW, "TW"},
    {meshtastic_Config_LoRaConfig_RegionCode_RU, "RU"},         {meshtastic_Config_LoRaConfig_RegionCode_IN, "IN"},
    {meshtastic_Config_LoRaConfig_RegionCode_NZ_865, "NZ_865"}, {meshtastic_Config_LoRaConfig_RegionCode_TH, "TH"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_433, "UA_433"}, {meshtastic_Config_LoRaConfig_RegionCode_UA_868, "UA_868"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_433, "MY_433"}, {meshtastic_Config_LoRaConfig_RegionCode_MY_919, "MY_919"},
    {meshtastic_Config_LoRaConfig_RegionCode_SG_923, "SG_923"}, {meshtastic_Config_LoRaConfig_RegionCode_PH_433, "PH_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_PH_868, "PH_868"}, {meshtastic_Config_LoRaConfig_RegionCode_PH_915, "PH_915"},
    {meshtastic_Config_LoRaConfig_RegionCode_ANZ_433, "ANZ_433"}, {meshtastic_Config_LoRaConfig_RegionCode_KZ_433, "KZ_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_KZ_863, "KZ_863"}, {meshtastic_Config_LoRaConfig_RegionCode_NP_865, "NP_865"},
    {meshtastic_Config_LoRaConfig_RegionCode_BR_902, "BR_902"}, {meshtastic_Config_LoRaConfig_RegionCode_LORA_24, "LORA_24"},
};
const Code PRESETS[] = {
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, "LONG_FAST"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW, "LONG_SLOW"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE, "LONG_MODERATE"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, "MEDIUM_SLOW"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST, "MEDIUM_FAST"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, "SHORT_SLOW"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST, "SHORT_FAST"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, "SHORT_TURBO"},
};

template <size_t N> const char *codeOf(const Code (&t)[N], int value)
{
    for (auto &c : t)
        if (c.value == value)
            return c.code;
    return nullptr;
}
template <size_t N> int valueOf(const Code (&t)[N], const char *code)
{
    for (auto &c : t)
        if (std::strcmp(c.code, code) == 0)
            return c.value;
    return -1;
}
int indexOf(const std::vector<Option> &opts, const char *code)
{
    for (size_t i = 0; code && i < opts.size(); i++)
        if (std::strcmp(opts[i].code, code) == 0)
            return (int)i;
    return -1;
}

const char *presetName(int preset)
{
    const char *code = codeOf(PRESETS, preset);
    int i = indexOf(lora_presets(), code);
    return i >= 0 ? lora_presets()[i].name : "LongFast";
}

const char *hwModelName(int hw)
{
    switch (hw) {
    case meshtastic_HardwareModel_T_DECK:
        return "T-Deck";
    case meshtastic_HardwareModel_T_DECK_PRO:
        return "T-Deck Pro";
    case meshtastic_HardwareModel_TBEAM:
        return "T-Beam";
    case meshtastic_HardwareModel_T_ECHO:
        return "T-Echo";
    case meshtastic_HardwareModel_RAK4631:
        return "RAK4631";
    case meshtastic_HardwareModel_RAK11310:
        return "RAK11310";
    case meshtastic_HardwareModel_HELTEC_V3:
        return "Heltec V3";
    case meshtastic_HardwareModel_HELTEC_V4:
        return "Heltec V4";
    case meshtastic_HardwareModel_HELTEC_WSL_V3:
        return "Heltec WSL V3";
    case meshtastic_HardwareModel_HELTEC_WIRELESS_TRACKER:
        return "Heltec Tracker";
    case meshtastic_HardwareModel_HELTEC_MESH_NODE_T114:
        return "Heltec T114";
    case meshtastic_HardwareModel_TRACKER_T1000_E:
        return "T1000-E";
    case meshtastic_HardwareModel_STATION_G2:
        return "Station G2";
    case meshtastic_HardwareModel_TLORA_T3_S3:
        return "T3-S3";
    case meshtastic_HardwareModel_NANO_G2_ULTRA:
        return "Nano G2 Ultra";
    case meshtastic_HardwareModel_SEEED_XIAO_S3:
        return "XIAO S3";
    case meshtastic_HardwareModel_SENSECAP_INDICATOR:
        return "SenseCAP Indicator";
    case meshtastic_HardwareModel_WISMESH_TAP:
        return "WisMesh Tap";
    case meshtastic_HardwareModel_T_WATCH_S3:
        return "T-Watch S3";
    case meshtastic_HardwareModel_T_LORA_PAGER:
        return "T-LoRa Pager";
    case meshtastic_HardwareModel_PORTDUINO:
        return "Linux";
    default:
        return "";
    }
}

// roles in the interface's closed vocabulary (translated when drawn)
const char *roleWord(uint8_t role)
{
    switch ((MeshtasticView::eRole)role) {
    case MeshtasticView::client_mute:
        return "cliente silencioso";
    case MeshtasticView::router:
    case MeshtasticView::router_client:
    case MeshtasticView::repeater:
    case MeshtasticView::router_late:
        return "repetidor";
    case MeshtasticView::tracker:
    case MeshtasticView::tak_tracker:
        return "rastreador";
    case MeshtasticView::sensor:
        return "sensor";
    default:
        return "cliente";
    }
}

bool isRelay(uint8_t role)
{
    auto r = (MeshtasticView::eRole)role;
    return r == MeshtasticView::router || r == MeshtasticView::router_client || r == MeshtasticView::repeater ||
           r == MeshtasticView::router_late;
}

uint32_t nowUtc(void)
{
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0; // before the clock is set: unknown
}

// minutes between local time and UTC at this moment (TZ comes from the device's time zone)
int tzOffsetMin(time_t t)
{
    struct tm lt, ut;
    localtime_r(&t, &lt);
    gmtime_r(&t, &ut);
    int d = (lt.tm_hour * 60 + lt.tm_min) - (ut.tm_hour * 60 + ut.tm_min);
    if (lt.tm_yday != ut.tm_yday)
        d += (lt.tm_year > ut.tm_year || (lt.tm_year == ut.tm_year && lt.tm_yday > ut.tm_yday)) ? 1440 : -1440;
    return d;
}

void copyText(char *dst, size_t size, const std::string &s)
{
    // cut on a character boundary, never in the middle of an accented letter
    size_t n = std::min(s.size(), size - 1);
    while (n > 0 && n < s.size() && ((uint8_t)s[n] & 0xC0) == 0x80)
        n--;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

void fillRandom(uint8_t *buf, size_t n)
{
#if defined(ARCH_PORTDUINO)
    std::random_device rd;
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)rd();
#else
    esp_fill_random(buf, n);
#endif
}

// ------------------------------------------------------------ map tiles from the SD card
// One layer per folder in /maps: z/x/y.png files or a <folder>.pmtiles archive.
class SdTiles : public TileSource
{
  public:
    SdTiles(IStorage &storage, const std::string &style, bool pmtiles)
        : storage(storage), style(style), label(style == "/map" ? "map" : style), archive(pmtiles)
    {
        if (archive)
            pm.reset(new PMTileService(storage));
    }
    ~SdTiles() override
    {
        for (auto &e : cache)
            release(e.second);
    }
    const char *name() const override { return label.c_str(); }

    const lv_image_dsc_t *tile(uint8_t z, uint32_t x, uint32_t y) override
    {
        uint64_t k = key(z, x, y);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == k) {
                cache.splice(cache.begin(), cache, it); // most recently used first
                return it->second;
            }
        if (missing.count(k) && missingGen == storage.generation())
            return nullptr;
        if (missingGen != storage.generation())
            missing.clear(), missingGen = storage.generation();
        lv_image_dsc_t *img = archive ? fromArchive(z, x, y) : fromFile(z, x, y);
        if (!img) {
            if (missing.size() > 400)
                missing.clear();
            missing.insert(k);
            return nullptr;
        }
        cache.emplace_front(k, img);
        trim();
        return img;
    }

    void retain(const std::vector<uint64_t> &keys) override
    {
        shown.assign(keys.begin(), keys.end());
        trim();
    }

  private:
    static constexpr size_t KEEP = 10; // 128 KB each, in PSRAM

    void trim(void)
    {
        while (cache.size() > KEEP) {
            auto victim = cache.end();
            for (auto it = cache.end(); it != cache.begin();) {
                --it;
                if (std::find(shown.begin(), shown.end(), it->first) == shown.end()) {
                    victim = it;
                    break;
                }
            }
            if (victim == cache.end())
                return;
            release(victim->second);
            cache.erase(victim);
        }
    }

    static void release(lv_image_dsc_t *img)
    {
        lv_image_cache_drop(img);
        if (img->data)
            lv_free((void *)img->data);
        lv_free(img);
    }

    std::string path(uint8_t z, uint32_t x, uint32_t y) const
    {
        char buf[96];
        if (style[0] == '/')
            snprintf(buf, sizeof(buf), "%s/%u/%u/%u.png", style.c_str(), z, (unsigned)x, (unsigned)y);
        else
            snprintf(buf, sizeof(buf), "%s/%s/%u/%u/%u.png", MapTileSettings::getPrefix(), style.c_str(), z, (unsigned)x,
                     (unsigned)y);
        return buf;
    }

    lv_image_dsc_t *fromFile(uint8_t z, uint32_t x, uint32_t y)
    {
        auto file = storage.open(path(z, x, y).c_str(), OpenMode::Read);
        if (!file)
            return nullptr;
        uint64_t size = file->size();
        if (size == 0 || size > 512 * 1024)
            return nullptr;
        uint8_t *png = (uint8_t *)lv_malloc((size_t)size);
        if (!png)
            return nullptr;
        bool ok = file->read(png, (uint32_t)size) == (int32_t)size;
        file.reset();
        lv_image_dsc_t *img = nullptr;
        if (!ok || !decodeImgColor(png, (size_t)size, &img))
            img = nullptr;
        lv_free(png);
        return img;
    }

    lv_image_dsc_t *fromArchive(uint8_t z, uint32_t x, uint32_t y)
    {
        // the service puts the decoded tile into an image object; take it from there
        if (!holder) {
            holder = lv_image_create(lv_layer_sys());
            lv_obj_set_hidden(holder, true);
        }
        MapTileSettings::setTileStyle(style.c_str());
        MapTileSettings::setPMTiles(true);
        if (!pm->load(path(z, x, y).c_str(), holder))
            return nullptr;
        auto *img = (lv_image_dsc_t *)lv_image_get_src(holder);
        lv_image_set_src(holder, nullptr);
        return img && lv_image_src_get_type(img) == LV_IMAGE_SRC_VARIABLE ? img : nullptr;
    }

    IStorage &storage;
    std::string style, label;
    bool archive;
    std::unique_ptr<PMTileService> pm;
    lv_obj_t *holder = nullptr;
    std::list<std::pair<uint64_t, lv_image_dsc_t *>> cache;
    std::vector<uint64_t> shown;
    std::set<uint64_t> missing;
    uint32_t missingGen = 0;
};

TFieldView *self = nullptr;
} // namespace

// ------------------------------------------------------------------ controller
bool TFieldController::sendData(uint32_t to, uint8_t ch, uint8_t hopLimit, uint32_t id, meshtastic_PortNum port, bool usePkc,
                                const uint8_t *bytes, size_t len)
{
    unsigned char buf[meshtastic_Constants_DATA_PAYLOAD_LEN] = {0};
    if (len > sizeof(buf))
        return false;
    std::memcpy(buf, bytes, len);
    return send(to, ch, hopLimit, id, port, false, usePkc, buf, len);
}

// ------------------------------------------------------------------------ view
TFieldView *TFieldView::gui = nullptr;

TFieldView *TFieldView::instance(void)
{
    if (!gui)
        gui = new TFieldView(nullptr, DisplayDriverFactory::create(320, 240));
    return gui;
}

TFieldView *TFieldView::instance(const DisplayDriverConfig &cfg)
{
    if (!gui)
        gui = new TFieldView(&cfg, DisplayDriverFactory::create(cfg));
    return gui;
}

TFieldView::TFieldView(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : MeshtasticView(cfg, driver, new TFieldController), ctl(static_cast<TFieldController *>(controller))
{
    self = this;
}

void TFieldView::init(IClientBase *client)
{
    ILOG_INFO("T-Field UI init");
    MeshtasticView::init(client);

    // settings and quick replies saved in the device
    Settings s;
    std::string text;
    if (readFlash(SETTINGS_FILE, text))
        s = settings_from_text(text);
    g_lang = s.lang ? Lang::En : Lang::Es; // build the screens in the saved language right away

    app = new App;
    connectHooks();
    app->init();
    app->set_settings(s);
    if (readFlash(QUICK_FILE, text)) {
        QuickReplies q = app->quick();
        size_t pos = 0;
        for (size_t i = 0; i < q.text.size() && pos <= text.size(); i++) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!line.empty())
                q.text[i] = line;
            pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        }
        app->set_quick(q);
    }
    // a track recorded with a faster GPS that did not get to restore it (power loss)
    if (readFlash(GPS_FILE, text) && std::atoi(text.c_str()) > 0)
        savedGpsInterval = (uint32_t)std::atoi(text.c_str());

    // SD card: tracks, waypoints and map tiles
    initPNGDecoder();
    storage = createStorage().release();
    sdMounted = storage && storage->mount() == IStorage::Status::Ok;
    if (sdMounted) {
        app->load_routes();
        app->load_waypoints();
        if (app->on_read_file) {
            std::string csv;
            if (app->on_read_file(App::TRACK_BACKUP, csv))
                app->recover_track(csv);
        }
        for (auto &style : MapStorage::loadMapStyles(*storage, MapTileSettings::getPrefix()))
            layers.push_back(new SdTiles(*storage, style,
                                         style[0] != '/' && MapStorage::hasMapArchive(*storage, MapTileSettings::getPrefix(),
                                                                                       style.c_str())));
        app->set_map_layers(layers);
    }

    // keyboard and trackball: through the interface first (Backspace goes back, the trackball
    // moves between items), then into LVGL by a keypad of our own
    keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, readKeys);
    if (inputdriver) {
        wrapInput(inputdriver->getKeyboard());
        wrapInput(inputdriver->getEncoder());
    }
    for (lv_indev_t *in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in))
        if (in != keypad && (lv_indev_get_type(in) == LV_INDEV_TYPE_KEYPAD || lv_indev_get_type(in) == LV_INDEV_TYPE_ENCODER))
            wrapInput(in);
    app->refresh_input_group();

    applySettings(true);
    uiStarted = true;
    state = MeshtasticView::eBootScreenDone; // the controller may ask the radio for its configuration now
    publishStatus(true);
}

void TFieldView::connectHooks(void)
{
    app->random_bytes = [](uint8_t *buf, size_t n) { fillRandom(buf, n); };

    app->on_send_text = [this](const std::string &chatId, const std::string &text) {
        sendText(chatId, text, app->last_sent_id(), 0);
    };

    app->on_sos = [this](bool, const std::string &text, const std::vector<std::string> &chatIds,
                         const std::vector<uint32_t> &direct) {
        for (auto &id : chatIds)
            sendText(id, text, 0, 0);
        for (uint32_t num : direct)
            sendText("node:" + std::to_string(num), text, 0, num);
    };

    app->on_share_waypoint = [this](const Waypoint &w, const Recipient &r) {
        if (r.channel) {
            int ch = channelOfChat(r.id);
            if (ch >= 0)
                shareWaypoint(BROADCAST, (uint8_t)ch, false, w.name, w.note, w.lat, w.lon);
        } else if (r.node_num) {
            auto it = nodeDb.find(r.node_num);
            bool pkc = it != nodeDb.end() && it->second.hasKey;
            shareWaypoint(r.node_num, it != nodeDb.end() ? it->second.channel : 0, pkc, w.name, w.note, w.lat, w.lon);
        }
    };

    app->on_request_position = [this](uint32_t num) {
        auto it = nodeDb.find(num);
        uint32_t id = requests.addRequest(num, ResponseHandler::PositionRequest, (void *)(uintptr_t)num);
        controller->requestPosition(num, it != nodeDb.end() ? it->second.channel : 0, id);
    };

    app->on_traceroute = [this](uint32_t num) {
        auto it = nodeDb.find(num);
        uint32_t id = requests.addRequest(num, ResponseHandler::TraceRouteRequest, (void *)(uintptr_t)num);
        controller->traceRoute(num, it != nodeDb.end() ? it->second.channel : 0, hopLimitFor(num), id);
    };

    app->on_settings = [this](const Settings &) {
        saveSettings();
        applySettings(false);
    };
    app->on_settings_preview = [this](const Settings &) { applySettings(true); };

    app->on_save_quick = [this](const QuickReplies &q) {
        std::string text;
        for (auto &t : q.text)
            text += t + "\n";
        writeFlash(QUICK_FILE, text);
    };

    app->on_recording = [this](bool on) { setRecording(on); };

    app->on_clear_chat = [this](const std::string &chatId, bool) {
        if (chatId.rfind("node:", 0) == 0)
            controller->removeTextMessages(ownNode, (uint32_t)std::strtoul(chatId.c_str() + 5, nullptr, 10), 0);
        else {
            int ch = channelOfChat(chatId);
            if (ch >= 0)
                controller->removeTextMessages(ownNode, UINT32_MAX, (uint8_t)ch);
        }
    };

    app->on_channel_set = [this](const std::string &name, const ChannelKey &key) {
        int index = teamChannelIndex;
        if (!name.empty()) {
            for (auto &c : app->chats())
                if (c.is_channel && c.name == name && c.channel > 0)
                    index = c.channel;
            if (index < 1) // first free secondary slot
                for (int i = 1; i < c_max_channels && index < 1; i++)
                    if (!channels[i].used)
                        index = i;
        }
        if (index < 1 || index >= c_max_channels) {
            ILOG_ERROR("T-Field: no free channel slot for the team channel");
            return;
        }
        meshtastic_Channel ch = meshtastic_Channel_init_default;
        ch.index = (int8_t)index;
        if (name.empty()) {
            ch.role = meshtastic_Channel_Role_DISABLED;
            channels[index] = Channel{};
            teamChannelIndex = -1;
        } else {
            ch.has_settings = true;
            copyText(ch.settings.name, sizeof(ch.settings.name), name);
            ch.settings.psk.size = (pb_size_t)key.size();
            std::memcpy(ch.settings.psk.bytes, key.data(), key.size());
            ch.role = meshtastic_Channel_Role_SECONDARY;
            channels[index] = Channel{true, false, name};
            teamChannelIndex = index;
        }
        controller->sendConfig(ch, ownNode);
    };

    app->on_channel_invite = [this](uint32_t num, const std::string &name, const ChannelKey &key) {
        auto it = nodeDb.find(num);
        if (it == nodeDb.end() || !it->second.hasKey)
            return; // only end-to-end encrypted: the key must not travel in the clear
        uint8_t buf[64];
        size_t n = 0;
        std::memcpy(buf, MAGIC, 3), n = 3;
        buf[n++] = PKT_INVITE;
        size_t len = std::min(name.size(), (size_t)CHANNEL_NAME_MAX_BYTES);
        buf[n++] = (uint8_t)len;
        std::memcpy(buf + n, name.data(), len), n += len;
        std::memcpy(buf + n, key.data(), key.size()), n += key.size();
        uint32_t id = requests.addRequest(num, ResponseHandler::anyRequest);
        ctl->sendData(num, it->second.channel, hopLimitFor(num), id, meshtastic_PortNum_PRIVATE_APP, true, buf, n);
    };

    app->on_channel_join_ack = [this](uint32_t num) {
        auto it = nodeDb.find(num);
        uint8_t buf[4] = {MAGIC[0], MAGIC[1], MAGIC[2], PKT_JOINED};
        uint32_t id = requests.addRequest(num, ResponseHandler::anyRequest);
        bool pkc = it != nodeDb.end() && it->second.hasKey;
        ctl->sendData(num, it != nodeDb.end() ? it->second.channel : 0, hopLimitFor(num), id, meshtastic_PortNum_PRIVATE_APP,
                      pkc, buf, sizeof(buf));
    };

    app->on_alert = [](Alert kind, const std::string &what) {
        // sound is played by the firmware's notification module (Meshtastic's ringtone setting)
        ILOG_DEBUG("T-Field alert %d: %s", (int)kind, what.c_str());
    };

    // SD card files (tracks, waypoints)
    app->on_read_file = [this](const std::string &path, std::string &content) {
        if (!sdReady())
            return false;
        auto f = storage->open(path.c_str(), OpenMode::Read);
        if (!f)
            return false;
        uint64_t size = f->size();
        if (size > 4 * 1024 * 1024)
            return false;
        content.resize((size_t)size);
        return size == 0 || f->read(&content[0], (uint32_t)size) == (int32_t)size;
    };
    auto writeSd = [this](const std::string &path, const std::string &content, OpenMode mode) {
        if (!sdReady())
            return false;
        size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            storage->mkdir(path.substr(0, slash).c_str());
        auto f = storage->open(path.c_str(), mode);
        if (!f)
            return false;
        return content.empty() || f->write(content.data(), (uint32_t)content.size()) == (int32_t)content.size();
    };
    app->on_save_file = [writeSd](const std::string &path, const std::string &content) {
        return writeSd(path, content, OpenMode::Write);
    };
    app->on_append_file = [writeSd](const std::string &path, const std::string &content) {
        return writeSd(path, content, OpenMode::Append);
    };
    app->on_delete_file = [this](const std::string &path) { return sdReady() && storage->remove(path.c_str()); };
}

// --------------------------------------------------------------------- sending
void TFieldView::sendText(const std::string &chatId, const std::string &text, uint32_t msgId, uint32_t sosNode)
{
    uint32_t to = BROADCAST;
    uint8_t ch = 0;
    bool pkc = false;
    uint8_t hops = hasLora && lora.hop_limit ? (uint8_t)lora.hop_limit : 3;
    if (chatId.rfind("node:", 0) == 0) {
        to = (uint32_t)std::strtoul(chatId.c_str() + 5, nullptr, 10);
        auto it = nodeDb.find(to);
        if (it != nodeDb.end())
            ch = it->second.channel, pkc = it->second.hasKey;
        hops = hopLimitFor(to);
    } else {
        int c = channelOfChat(chatId);
        if (c < 0) {
            if (msgId)
                app->set_message_status(msgId, "No enviado");
            return;
        }
        ch = (uint8_t)c;
    }
    uint32_t id = requests.addRequest(to, ResponseHandler::TextMessageRequest, (void *)(uintptr_t)to);
    if (sentText.size() > 64) // a status that never came (a broadcast nobody repeated)
        sentText.erase(sentText.begin());
    if (sosText.size() > 64)
        sosText.erase(sosText.begin());
    if (msgId)
        sentText[id] = msgId;
    if (sosNode)
        sosText[id] = sosNode;
    std::string t = text.substr(0, std::min(text.size(), (size_t)meshtastic_Constants_DATA_PAYLOAD_LEN - 1));
    controller->sendTextMessage(to, ch, hops, nowUtc() ? nowUtc() : (uint32_t)time(nullptr), id, pkc, t.c_str());
}

void TFieldView::shareWaypoint(uint32_t to, uint8_t ch, bool pkc, const std::string &name, const std::string &note, double lat,
                               double lon)
{
    meshtastic_Waypoint wp = meshtastic_Waypoint_init_default;
    fillRandom((uint8_t *)&wp.id, sizeof(wp.id));
    wp.id |= 1;
    wp.has_latitude_i = wp.has_longitude_i = true;
    wp.latitude_i = (int32_t)std::lround(lat * 1e7);
    wp.longitude_i = (int32_t)std::lround(lon * 1e7);
    wp.expire = 0; // does not expire
    copyText(wp.name, sizeof(wp.name), name);
    copyText(wp.description, sizeof(wp.description), note);
    wp.icon = 0x1F4CD; // round pushpin
    uint8_t buf[meshtastic_Constants_DATA_PAYLOAD_LEN];
    size_t n = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_Waypoint_msg, &wp);
    if (!n)
        return;
    uint32_t id = requests.addRequest(to, ResponseHandler::anyRequest);
    ctl->sendData(to, ch, to == BROADCAST ? (hasLora ? (uint8_t)lora.hop_limit : 3) : hopLimitFor(to), id,
                  meshtastic_PortNum_WAYPOINT_APP, pkc, buf, n);
}

void TFieldView::setRecording(bool on)
{
    // a track needs the GPS every few seconds; the firmware's default is every two minutes
    if (!hasPosCfg)
        return;
    uint32_t want = std::max<uint32_t>(5, app->settings().track_every_s);
    if (on && !savedGpsInterval && posCfg.gps_update_interval != want) {
        savedGpsInterval = posCfg.gps_update_interval ? posCfg.gps_update_interval : 120;
        writeFlash(GPS_FILE, std::to_string(savedGpsInterval));
        meshtastic_Config_PositionConfig c = posCfg;
        c.gps_update_interval = want;
        posCfg = c;
        controller->sendConfig(std::move(c), ownNode);
    } else if (!on && savedGpsInterval) {
        meshtastic_Config_PositionConfig c = posCfg;
        c.gps_update_interval = savedGpsInterval;
        posCfg = c;
        savedGpsInterval = 0;
        writeFlash(GPS_FILE, "0");
        controller->sendConfig(std::move(c), ownNode);
    }
}

// -------------------------------------------------------------------- settings
void TFieldView::saveSettings(void)
{
    writeFlash(SETTINGS_FILE, settings_to_text(app->settings()));
}

void TFieldView::applySettings(bool preview)
{
    const Settings &s = app->settings();
    // display and keyboard: right away, also while browsing the options
    if (displaydriver) {
        if (displaydriver->hasLight())
            displaydriver->setBrightness((uint8_t)std::min(255, std::max(10, (int)s.brightness) * 255 / 100));
        displaydriver->setScreenTimeout(s.screen_off_s);
    }
#if defined(T_DECK) && !defined(ARCH_PORTDUINO)
    Wire.beginTransmission(0x55); // T-Deck keyboard: backlight command
    Wire.write(0x01);
    Wire.write(s.kbd_light ? 127 : 0);
    Wire.endTransmission();
#endif
    if (preview)
        return;

    // what belongs to Meshtastic: only what changed since the radio reported it, so a value
    // this interface cannot show (another region, a custom time zone) is left alone
    if (!haveApplied)
        return;
    if (s.short_name != applied.short_name || s.long_name != applied.long_name) {
        meshtastic_User u = ownUser;
        copyText(u.short_name, sizeof(u.short_name), s.short_name);
        copyText(u.long_name, sizeof(u.long_name), s.long_name);
        ownUser = u;
        controller->sendConfig(u, ownNode);
    }
    if (hasPosCfg && (s.gps != applied.gps || s.pos_every_s != applied.pos_every_s || s.pos_smart != applied.pos_smart)) {
        meshtastic_Config_PositionConfig c = posCfg;
        c.gps_mode = s.gps ? meshtastic_Config_PositionConfig_GpsMode_ENABLED : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
        c.position_broadcast_secs = s.pos_every_s;
        c.position_broadcast_smart_enabled = s.pos_smart;
        posCfg = c;
        controller->sendConfig(std::move(c), ownNode);
    }
    if (hasDevCfg && s.tz != applied.tz && s.tz < time_zones().size()) {
        meshtastic_Config_DeviceConfig c = devCfg;
        copyText(c.tzdef, sizeof(c.tzdef), time_zones()[s.tz].code);
        devCfg = c;
        setenv("TZ", c.tzdef, 1);
        tzset();
        controller->sendConfig(std::move(c), ownNode);
        publishStatus(true);
    }
    if (hasLora && (s.region != applied.region || s.preset != applied.preset || s.hops != applied.hops)) {
        meshtastic_Config_LoRaConfig c = lora;
        if (s.region != applied.region && s.region < lora_regions().size()) {
            int r = valueOf(REGIONS, lora_regions()[s.region].code);
            if (r >= 0)
                c.region = (meshtastic_Config_LoRaConfig_RegionCode)r;
        }
        if (s.preset != applied.preset && s.preset < lora_presets().size()) {
            int p = valueOf(PRESETS, lora_presets()[s.preset].code);
            if (p >= 0)
                c.use_preset = true, c.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)p;
        }
        c.hop_limit = std::min<uint32_t>(7, std::max<uint32_t>(1, s.hops));
        lora = c;
        controller->sendConfig(std::move(c), ownNode); // the radio restarts
    }
    applied = s;
}

// The radio is the authority for its own settings: names, GPS, time zone and LoRa.
void TFieldView::mergeFirmwareSettings(void)
{
    Settings s = app->settings();
    if (hasOwnUser) {
        if (ownUser.short_name[0])
            s.short_name = ownUser.short_name;
        if (ownUser.long_name[0])
            s.long_name = ownUser.long_name;
    }
    if (hasPosCfg) {
        s.gps = posCfg.gps_mode != meshtastic_Config_PositionConfig_GpsMode_DISABLED;
        if (posCfg.position_broadcast_secs)
            s.pos_every_s = (uint16_t)std::min<uint32_t>(65535, posCfg.position_broadcast_secs);
        s.pos_smart = posCfg.position_broadcast_smart_enabled;
    }
    if (hasDevCfg && devCfg.tzdef[0]) {
        int i = indexOf(time_zones(), devCfg.tzdef);
        if (i >= 0)
            s.tz = (uint8_t)i;
    }
    if (hasLora) {
        int i = indexOf(lora_regions(), codeOf(REGIONS, lora.region));
        if (i >= 0)
            s.region = (uint8_t)i;
        if (lora.use_preset) {
            i = indexOf(lora_presets(), codeOf(PRESETS, lora.modem_preset));
            if (i >= 0)
                s.preset = (uint8_t)i;
        }
        if (lora.hop_limit)
            s.hops = (uint8_t)lora.hop_limit;
    }
    app->set_settings(s);
    applied = s;
    haveApplied = hasOwnUser || hasPosCfg || hasDevCfg || hasLora;
}

// ------------------------------------------------------------- startup states
bool TFieldView::setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig)
{
    // T-Field keeps its own settings; nothing to take from the stock UI's configuration
    if (state < MeshtasticView::eSetupUIConfig)
        state = MeshtasticView::eSetupUIConfig;
    return true;
}

void TFieldView::configCompleted(void)
{
    MeshtasticView::configCompleted();
    if (!chatsSeeded) {
        // a chat for each channel, as the radio has them
        std::vector<Chat> chats;
        for (int i = 0; i < c_max_channels; i++) {
            if (!channels[i].used)
                continue;
            Chat c;
            c.id = channelChatId((uint8_t)i);
            c.name = channelName((uint8_t)i);
            c.is_channel = true;
            c.channel = (int8_t)i;
            chats.push_back(c);
        }
        app->set_chats(chats);
        chatsSeeded = true;
    }
    if (savedGpsInterval && hasPosCfg && !app->track().recording)
        setRecording(false); // restore the GPS interval a power loss left behind
    mergeFirmwareSettings(); // also the baseline for later changes
    teamDirty = true;
    publishStatus(true);
}

// ------------------------------------------------------------------ node data
void TFieldView::setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t)
{
    ownNode = nodeNum;
}

void TFieldView::setDeviceMetaData(int hw_model, const char *version, bool, bool, bool, bool)
{
    hwName = hwModelName(hw_model);
    fwVersion = version ? version : "";
    publishStatus(true);
}

void TFieldView::addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg)
{
    Node &n = nodeDb[nodeNum];
    n.channel = channel;
    if (lastHeard)
        n.lastHeard = lastHeard;
    updateNode(nodeNum, -1, cfg);
}

void TFieldView::addNode(uint32_t nodeNum, uint8_t channel, const char *userShort, const char *userLong, uint32_t lastHeard,
                         eRole role, bool hasKey, bool)
{
    Node &n = nodeDb[nodeNum];
    n.channel = channel;
    n.shortName = userShort ? userShort : "";
    n.longName = userLong ? userLong : "";
    n.role = (uint8_t)role;
    n.hasKey = hasKey;
    if (lastHeard)
        n.lastHeard = lastHeard;
    teamDirty = true;
}

void TFieldView::updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg)
{
    Node &n = nodeDb[nodeNum];
    if (channel != (uint8_t)-1)
        n.channel = channel;
    n.shortName = cfg.short_name;
    n.longName = cfg.long_name;
    n.role = (uint8_t)cfg.role;
    n.hwModel = cfg.hw_model;
    n.hasKey = cfg.public_key.size == 32;
    if (nodeNum == ownNode) {
        ownUser = cfg;
        bool first = !hasOwnUser;
        hasOwnUser = true;
        if (!first && uiStarted && configComplete)
            mergeFirmwareSettings();
        publishStatus(true);
    }
    teamDirty = true;
}

void TFieldView::removeNode(uint32_t nodeNum)
{
    nodeDb.erase(nodeNum);
    teamDirty = true;
}

bool TFieldView::hasKnownNodeForPacket(uint32_t nodeNum) const
{
    return nodeDb.count(nodeNum) != 0;
}

void TFieldView::updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision)
{
    if (lat == 0 && lon == 0)
        return;
    Node &n = nodeDb[nodeNum];
    n.hasPos = true;
    n.lat = lat * 1e-7;
    n.lon = lon * 1e-7;
    n.alt = alt;
    n.posTime = nowUtc();
    if (precision > 0 && precision < 32) // bits kept: a coarse position for privacy
        n.accuracy = (uint16_t)std::min(65535.0, 23300000.0 / std::pow(2.0, (double)precision));
    if (nodeNum == ownNode) {
        if (sats)
            ownSats = (uint8_t)sats;
        if (!localPosTime) // without the firmware's live fix: the reported one
            publishOwnPosition(n.lat, n.lon, alt, n.posTime, n.accuracy);
    } else if (uiStarted) {
        TrackPoint p;
        p.lat = n.lat, p.lon = n.lon, p.alt = (float)alt, p.epoch = n.posTime;
        app->add_trail_point(nodeNum, p);
    }
    teamDirty = true;
}

void TFieldView::updateMetrics(uint32_t nodeNum, const meshtastic_DeviceMetrics &m)
{
    Node &n = nodeDb[nodeNum];
    if (m.has_battery_level)
        n.battery = (int8_t)std::min<uint32_t>(101, m.battery_level);
    if (m.has_voltage)
        n.voltage = m.voltage;
    if (nodeNum == ownNode && m.has_battery_level)
        ownBattery = n.battery;
    teamDirty = true;
}

void TFieldView::updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr)
{
    Node &n = nodeDb[nodeNum];
    n.rssi = (int16_t)rssi;
    n.snr = snr;
    n.hops = 0;
    teamDirty = true;
}

void TFieldView::updateHopsAway(uint32_t nodeNum, uint8_t hopsAway)
{
    nodeDb[nodeNum].hops = (int8_t)hopsAway;
    teamDirty = true;
}

void TFieldView::updateLastHeard(uint32_t nodeNum)
{
    uint32_t t = nowUtc();
    if (t)
        nodeDb[nodeNum].lastHeard = t;
    teamDirty = true;
}

// ---------------------------------------------------------------- radio config
void TFieldView::updateChannelConfig(const meshtastic_Channel &ch)
{
    if (ch.index < 0 || ch.index >= c_max_channels)
        return;
    Channel &c = channels[ch.index];
    c.used = ch.role != meshtastic_Channel_Role_DISABLED;
    c.primary = ch.role == meshtastic_Channel_Role_PRIMARY;
    c.name = ch.has_settings ? ch.settings.name : "";
    const Settings &s = app->settings();
    if (c.used && s.channel.on && c.name == s.channel.name)
        teamChannelIndex = ch.index;
}

void TFieldView::updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg)
{
    devCfg = cfg;
    hasDevCfg = true;
    if (cfg.tzdef[0]) {
        setenv("TZ", cfg.tzdef, 1);
        tzset();
    }
}

void TFieldView::updatePositionConfig(const meshtastic_Config_PositionConfig &cfg)
{
    posCfg = cfg;
    hasPosCfg = true;
}

void TFieldView::updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg)
{
    lora = cfg;
    hasLora = true;
}

// -------------------------------------------------------------------- messages
std::string TFieldView::channelName(uint8_t ch) const
{
    const Channel &c = channels[ch];
    if (!c.name.empty())
        return c.name;
    if (c.primary || ch == 0) // the public channel is named after the modem preset
        return presetName(hasLora && lora.use_preset ? lora.modem_preset : 0);
    return "Canal " + std::to_string(ch);
}

std::string TFieldView::channelChatId(uint8_t ch) const
{
    const Settings &s = app->settings();
    if (s.channel.on && !s.channel.chat_id.empty() && (int)ch == teamChannelIndex)
        return s.channel.chat_id;
    return "ch:" + std::to_string(ch);
}

int TFieldView::channelOfChat(const std::string &chatId) const
{
    for (auto &c : app->chats())
        if (c.id == chatId && c.is_channel)
            return c.channel >= 0 ? c.channel : 0;
    if (chatId.rfind("ch:", 0) == 0)
        return std::atoi(chatId.c_str() + 3);
    return -1;
}

std::string TFieldView::chatFor(uint32_t from, uint32_t to, uint8_t ch)
{
    if (to == ownNode && from != ownNode)
        return "node:" + std::to_string(from);
    if (from == ownNode && to != BROADCAST)
        return "node:" + std::to_string(to);
    return channelChatId(ch);
}

std::string TFieldView::nodeName(uint32_t num) const
{
    auto it = nodeDb.find(num);
    if (it != nodeDb.end() && !it->second.shortName.empty())
        return it->second.shortName;
    char buf[12];
    snprintf(buf, sizeof(buf), "!%04x", (unsigned)(num & 0xffff));
    return buf;
}

uint32_t TFieldView::nodeColor32(uint32_t num) const
{
    // the interface's identity colors: dark enough for white text on both themes
    static const uint32_t palette[] = {0x3B2E8F, 0x1F63B5, 0x2E6B34, 0x8A5A00, 0x0E7C86, 0x9C2F6E, 0x5B4A3A, 0x6A3FB5};
    uint32_t h = num * 2654435761u;
    return palette[(h >> 24) % (sizeof(palette) / sizeof(palette[0]))];
}

uint8_t TFieldView::hopLimitFor(uint32_t num) const
{
    uint8_t max = hasLora && lora.hop_limit ? (uint8_t)lora.hop_limit : 3;
    auto it = nodeDb.find(num);
    int hops = it != nodeDb.end() ? it->second.hops : -1;
    if (hops < 0)
        return max;
    return (uint8_t)std::min<int>(max, hops + 1);
}

std::string TFieldView::localHHMM(uint32_t epoch) const
{
    time_t t = epoch ? (time_t)epoch : time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return buf;
}

void TFieldView::newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, bool restore)
{
    if (!app || !msg)
        return;
    Message m;
    m.mine = from == ownNode;
    m.from = m.mine ? tr("T\xC3\xBA") : nodeName(from);
    m.from_color = m.mine ? 0 : nodeColor32(from);
    m.text = msg;
    m.epoch = msgtime > 1600000000 ? msgtime : nowUtc();
    m.time = localHHMM(m.epoch);
    if (m.mine)
        m.status = "Enviado";
    app->add_message(chatFor(from, to, ch), m, !restore);
}

void TFieldView::restoreMessage(const LogMessage &msg)
{
    if (!app || msg.trashFlag || msg.status == LogMessage::eDeleted || msg._size == 0)
        return;
    Message m;
    m.mine = msg.from == ownNode;
    m.from = m.mine ? tr("T\xC3\xBA") : nodeName(msg.from);
    m.from_color = m.mine ? 0 : nodeColor32(msg.from);
    m.text.assign((const char *)msg.bytes, msg._size);
    m.epoch = msg.time > 1600000000 ? (uint32_t)msg.time : 0;
    m.time = m.epoch ? localHHMM(m.epoch) : "";
    if (m.mine)
        m.status = msg.status == LogMessage::eAcked     ? "Entregado"
                   : msg.status == LogMessage::eFailed  ? "No enviado"
                   : msg.status == LogMessage::eNoResponse ? "Sin confirmar"
                                                          : "Enviado";
    app->add_message(chatFor(msg.from, msg.to, msg.ch), m, false);
}

void TFieldView::textMessageStatus(uint32_t requestId, LogMessage::MsgStatus status)
{
    auto sos = sosText.find(requestId);
    if (sos != sosText.end()) {
        if (status == LogMessage::eAcked)
            app->sos_ack(sos->second);
        if (status != LogMessage::eHeard)
            sosText.erase(sos);
    }
    auto it = sentText.find(requestId);
    if (it == sentText.end())
        return;
    switch (status) {
    case LogMessage::eAcked:
        app->set_message_status(it->second, "Entregado");
        break;
    case LogMessage::eNoResponse:
        app->set_message_status(it->second, "Sin confirmar");
        break;
    case LogMessage::eFailed:
        app->set_message_status(it->second, "No enviado");
        break;
    default:
        return; // heard by a repeater: still on its way
    }
    sentText.erase(it);
    requests.removeRequest(requestId);
}

void TFieldView::packetReceived(const meshtastic_MeshPacket &p)
{
    MeshtasticView::packetReceived(p);
    if (!app || p.which_payload_variant != meshtastic_MeshPacket_decoded_tag || p.from == ownNode)
        return;
    const auto &d = p.decoded;
    if (d.portnum == meshtastic_PortNum_WAYPOINT_APP) {
        meshtastic_Waypoint wp = meshtastic_Waypoint_init_default;
        if (!pb_decode_from_bytes(d.payload.bytes, d.payload.size, &meshtastic_Waypoint_msg, &wp))
            return;
        uint32_t now = nowUtc();
        if (!wp.has_latitude_i || !wp.has_longitude_i || (wp.expire && now && wp.expire < now))
            return; // a deleted or expired waypoint
        Waypoint w;
        w.lat = wp.latitude_i * 1e-7;
        w.lon = wp.longitude_i * 1e-7;
        w.name = wp.name;
        w.note = wp.description;
        w.from = nodeName(p.from);
        w.epoch = now;
        w.time = localHHMM(now);
        app->receive_waypoint(w, chatFor(p.from, p.to, p.channel));
    } else if (d.portnum == meshtastic_PortNum_PRIVATE_APP && d.payload.size >= 4 &&
               std::memcmp(d.payload.bytes, MAGIC, 3) == 0 && p.to == ownNode) {
        uint8_t type = d.payload.bytes[3];
        if (type == PKT_INVITE && p.pki_encrypted && d.payload.size >= 5) {
            size_t len = d.payload.bytes[4];
            if (len == 0 || len > CHANNEL_NAME_MAX_BYTES || d.payload.size != 5 + len + 32)
                return;
            ChannelKey key;
            std::memcpy(key.data(), d.payload.bytes + 5 + len, key.size());
            app->receive_channel_invite(p.from, std::string((const char *)d.payload.bytes + 5, len), key);
        } else if (type == PKT_JOINED) {
            app->channel_joined(p.from);
        }
    }
}

void TFieldView::handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route)
{
    auto req = requests.findRequest(id);
    if (!app || req.type != ResponseHandler::TraceRouteRequest)
        return;
    uint32_t target = (uint32_t)(uintptr_t)req.cookie;
    std::string text = nodeName(ownNode);
    for (pb_size_t i = 0; i < route.route_count && i < 8; i++)
        text += " \xE2\x80\xBA " + nodeName(route.route[i]);
    text += " \xE2\x80\xBA " + nodeName(target);
    text += ", " + format_hops((int)route.route_count);
    if (route.snr_towards_count > 0) {
        char buf[32];
        double snr = route.snr_towards[route.snr_towards_count - 1] / 4.0;
        snprintf(buf, sizeof(buf), " \xC2\xB7 SNR %.1f dB", snr);
        std::string s = buf;
        if (!english())
            std::replace(s.begin(), s.end(), '.', ',');
        text += s;
    }
    app->traceroute_result(target, text);
    requests.removeRequest(id);
}

void TFieldView::handlePositionResponse(uint32_t from, uint32_t request_id, int32_t, float, bool)
{
    if (app)
        app->position_received(from);
    requests.removeRequest(request_id);
}

// ------------------------------------------------------- status and team lists
void TFieldView::publishOwnPosition(double lat, double lon, int32_t alt, uint32_t epoch, uint16_t acc)
{
    havePos = true;
    ownLat = lat, ownLon = lon, ownAlt = alt;
    if (!app)
        return;
    Position p;
    p.valid = true;
    p.lat = lat, p.lon = lon, p.alt = alt, p.acc_m = acc;
    app->set_position(p);
    TrackPoint t;
    t.lat = lat, t.lon = lon, t.alt = (float)alt, t.epoch = epoch ? epoch : nowUtc();
    app->add_track_point(t);
    teamDirty = true;
}

void TFieldView::pollLocalPosition(void)
{
    if (&localPosition == nullptr)
        return;
    // only the leading fields: the same in every protobuf version
    uint32_t t = localPosition.time;
    int32_t lat = localPosition.latitude_i, lon = localPosition.longitude_i;
    if (!t || t == localPosTime || (lat == 0 && lon == 0))
        return;
    localPosTime = t;
    publishOwnPosition(lat * 1e-7, lon * 1e-7, localPosition.altitude, t, 0);
}

void TFieldView::publishStatus(bool force)
{
    if (!app)
        return;
    uint32_t now = nowUtc();
    time_t t = now ? (time_t)now : time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    uint32_t minute = (uint32_t)(t / 60);
    std::string name = hasOwnUser ? ownUser.short_name : "";
    bool fix = havePos && (localPosTime ? now - localPosTime < 300 : true);
    if (!force && minute == lastStatusMinute && ownBattery == lastBattery && ownSats == lastSats && fix == lastFix &&
        name == lastName)
        return;
    lastStatusMinute = minute, lastBattery = ownBattery, lastSats = ownSats, lastFix = fix, lastName = name;

    Status s;
    s.epoch = now;
    s.hour = (uint8_t)lt.tm_hour;
    s.minute = (uint8_t)lt.tm_min;
    s.battery = ownBattery;
    s.sats = ownSats;
    s.gps_fix = fix;
    s.short_name = name;
    char id[12];
    snprintf(id, sizeof(id), "!%08x", (unsigned)ownNode);
    s.node_id = ownNode ? id : "";
    s.hw = hwName;
    s.firmware = fwVersion;
    if (now && havePos) {
        DaylightInfo d = daylight_info(now, tzOffsetMin(t), ownLat, ownLon);
        if (d.valid) {
            s.sunrise = d.sunrise;
            s.sunset = d.sunset;
            s.daylight_min = d.minutes_left;
        }
    }
    app->set_status(s);
}

void TFieldView::publishTeam(void)
{
    uint32_t now = nowUtc();
    std::vector<TeamNode> team;
    team.reserve(nodeDb.size());
    for (auto &kv : nodeDb) {
        if (kv.first == ownNode || kv.first == 0)
            continue;
        const Node &n = kv.second;
        TeamNode t;
        t.num = kv.first;
        t.name = nodeName(kv.first);
        t.long_name = n.longName;
        t.color = nodeColor32(kv.first);
        t.role = roleWord(n.role);
        t.relay = isRelay(n.role);
        t.hw = hwModelName(n.hwModel);
        t.has_pki = n.hasKey;
        t.battery = n.battery;
        t.voltage = n.voltage;
        t.snr = n.snr;
        t.rssi = n.rssi;
        t.hops = n.hops;
        t.heard_ago_s = n.lastHeard && now > n.lastHeard ? now - n.lastHeard : (n.lastHeard ? 0 : 0xffffffff);
        t.stale = !n.lastHeard || t.heard_ago_s > secs_until_offline;
        if (n.hops == 0 && n.rssi != 0)
            t.signal = n.snr > 5 ? 4 : n.snr > 0 ? 3 : n.snr > -7 ? 2 : 1;
        else if (n.hops > 0)
            t.signal = 1;
        if (n.hasPos) {
            t.has_pos = true;
            t.lat = n.lat, t.lon = n.lon;
            t.acc_m = n.accuracy;
            t.pos_age_s = n.posTime && now > n.posTime ? now - n.posTime : 0;
            if (havePos) {
                t.dist_m = (int32_t)std::lround(haversine_m(ownLat, ownLon, n.lat, n.lon));
                t.bearing_deg = (int16_t)std::lround(bearing_deg(ownLat, ownLon, n.lat, n.lon));
                t.has_alt = n.alt != 0;
                t.alt_diff_m = n.alt - ownAlt;
            }
        }
        team.push_back(t);
    }
    // nearest first; without a position, the most recently heard
    std::sort(team.begin(), team.end(), [](const TeamNode &a, const TeamNode &b) {
        if ((a.dist_m >= 0) != (b.dist_m >= 0))
            return a.dist_m >= 0;
        if (a.dist_m >= 0)
            return a.dist_m < b.dist_m;
        return a.heard_ago_s < b.heard_ago_s;
    });
    app->set_team(team);
}

void TFieldView::screenSaving(bool enabled)
{
    if (!enabled)
        publishStatus(true);
}

void TFieldView::task_handler(void)
{
    MeshtasticView::task_handler();
    if (!uiStarted)
        return;
    uint32_t ms = lv_tick_get();
    if (ms - lastPoll >= 1000) {
        lastPoll = ms;
        pollLocalPosition();
        publishStatus(false);
        uint32_t now = (uint32_t)(ms / 1000);
        // the team: when something changed (at most once a second), and every minute for "5 min ago"
        if ((teamDirty && configComplete) || now - lastTeam >= 60) {
            teamDirty = false;
            lastTeam = now;
            publishTeam();
        }
    }
}

// ----------------------------------------------------------------------- input
void TFieldView::wrapInput(lv_indev_t *indev)
{
    if (!indev || indev == keypad || hwRead.count(indev))
        return;
    lv_indev_read_cb_t cb = lv_indev_get_read_cb(indev);
    if (!cb)
        return;
    hwRead[indev] = cb;
    lv_indev_set_read_cb(indev, readHardware);
}

void TFieldView::pushKey(uint32_t key, bool pressed)
{
    static uint32_t swallowed = 0;
    if (pressed) {
        if (displaydriver && displaydriver->isPowersaving()) {
            lv_display_trigger_activity(lv_display_get_default()); // the first key only wakes the screen
            swallowed = key;
            return;
        }
        if (app && app->handle_key(key)) {
            swallowed = key;
            return;
        }
        swallowed = 0;
        keyQueue.push_back({key, true});
        lv_display_trigger_activity(lv_display_get_default());
    } else {
        if (key == swallowed) {
            swallowed = 0;
            return;
        }
        keyQueue.push_back({key, false});
    }
}

// Reads the hardware (keyboard, trackball) and hands its keys to the interface.
void TFieldView::readHardware(lv_indev_t *indev, lv_indev_data_t *data)
{
    TFieldView *v = gui;
    auto it = v->hwRead.find(indev);
    lv_indev_data_t raw;
    std::memset(&raw, 0, sizeof(raw));
    raw.state = LV_INDEV_STATE_RELEASED;
    if (it != v->hwRead.end())
        it->second(indev, &raw);
    auto &st = v->hwState[indev]; // key held, pressed
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        // trackball: up/down move between items, left/right change a value, click is Enter
        for (int i = 0; i < std::abs((int)raw.enc_diff) && i < 4; i++) {
            uint32_t k = raw.enc_diff < 0 ? LV_KEY_UP : LV_KEY_DOWN;
            v->pushKey(k, true);
            v->pushKey(k, false);
        }
        if (raw.state == LV_INDEV_STATE_PRESSED && raw.key == LV_KEY_ENTER) {
            if (!st.second)
                v->pushKey(LV_KEY_ENTER, true), st = {LV_KEY_ENTER, true};
        } else if (raw.state == LV_INDEV_STATE_PRESSED && (raw.key == LV_KEY_UP || raw.key == LV_KEY_DOWN)) {
            uint32_t k = raw.key == LV_KEY_DOWN ? LV_KEY_LEFT : LV_KEY_RIGHT; // the driver sends left as "down"
            v->pushKey(k, true);
            v->pushKey(k, false);
        } else if (st.second && st.first == LV_KEY_ENTER && raw.state == LV_INDEV_STATE_RELEASED) {
            v->pushKey(LV_KEY_ENTER, false);
            st = {0, false};
        }
        data->enc_diff = 0;
    } else {
        if (raw.state == LV_INDEV_STATE_PRESSED && raw.key) {
            if (!st.second || st.first != raw.key) {
                if (st.second)
                    v->pushKey(st.first, false);
                v->pushKey(raw.key, true);
                st = {raw.key, true};
            }
        } else if (st.second) {
            v->pushKey(st.first, false);
            st = {0, false};
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;
}

// The keypad LVGL listens to: the keys the interface did not use.
void TFieldView::readKeys(lv_indev_t *, lv_indev_data_t *data)
{
    TFieldView *v = gui;
    if (!v->keyQueue.empty()) {
        KeyEvent ev = v->keyQueue.front();
        v->keyQueue.erase(v->keyQueue.begin());
        if (ev.pressed)
            v->curKey = ev.key, v->curPressed = true;
        else if (ev.key == v->curKey)
            v->curPressed = false;
    }
    data->key = v->curKey;
    data->state = v->curPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !v->keyQueue.empty();
}

// ----------------------------------------------------------------------- files
bool TFieldView::readFlash(const char *path, std::string &out)
{
    File f = persistentFS.open(path, "r");
    if (!f)
        return false;
    out.clear();
    char buf[256];
    size_t n;
    while ((n = f.read((uint8_t *)buf, sizeof(buf))) > 0)
        out.append(buf, n);
    f.close();
    return true;
}

bool TFieldView::writeFlash(const char *path, const std::string &content)
{
    persistentFS.mkdir("/tfield");
    File f = persistentFS.open(path, "w");
    if (!f)
        return false;
    bool ok = f.write((const uint8_t *)content.data(), content.size()) == content.size();
    f.close();
    return ok;
}

bool TFieldView::sdReady(void)
{
    static uint32_t lastTry = 0;
    if (!storage)
        return false;
    if (!sdMounted && lv_tick_elaps(lastTry) > 10000) { // card inserted later
        lastTry = lv_tick_get();
        sdMounted = storage->mount() == IStorage::Status::Ok;
    }
    return sdMounted;
}

#endif
