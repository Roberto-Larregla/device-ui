#pragma once

#include "graphics/common/MeshtasticView.h"
#include "graphics/common/ViewController.h"
#include "tfield/settings.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tfield
{
class App;
class TileSource;
} // namespace tfield

class IStorage;

/**
 * Controller with a raw send, for the packets T-Field UI sends besides text
 * (waypoints and the private-channel invitations).
 */
class TFieldController : public ViewController
{
  public:
    bool sendData(uint32_t to, uint8_t ch, uint8_t hopLimit, uint32_t id, meshtastic_PortNum port, bool usePkc,
                  const uint8_t *bytes, size_t len);
};

/**
 * T-Field UI (https://github.com/Roberto-Larregla/t-field-ui) on top of device-ui.
 *
 * T-Field UI does not know about Meshtastic: this view is the adapter. It feeds the
 * interface with what the controller reports (nodes, positions, messages, configuration)
 * and turns the interface's hooks into packets and configuration changes.
 *
 * Its own settings and quick replies are kept in the device's flash (/tfield/ajustes.txt,
 * /tfield/respuestas.txt); tracks and waypoints go to the SD card (/tfield/...); map tiles
 * are read from /maps on the SD card, as with the stock view.
 */
class TFieldView : public MeshtasticView
{
  public:
    void init(IClientBase *client) override;
    void task_handler(void) override;

    bool setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig) override;
    void setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t device_id) override;
    void setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                           bool can_shutdown) override;
    void addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg) override;
    void addNode(uint32_t nodeNum, uint8_t channel, const char *userShort, const char *userLong, uint32_t lastHeard, eRole role,
                 bool hasKey, bool viaMqtt) override;
    void updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg) override;
    void updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision) override;
    void updateMetrics(uint32_t nodeNum, const meshtastic_DeviceMetrics &metrics) override;
    void updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr) override;
    void updateHopsAway(uint32_t nodeNum, uint8_t hopsAway) override;
    void updateLastHeard(uint32_t nodeNum) override;
    void removeNode(uint32_t nodeNum) override;
    bool hasKnownNodeForPacket(uint32_t nodeNum) const override;

    void updateChannelConfig(const meshtastic_Channel &ch) override;
    void updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg) override;
    void updatePositionConfig(const meshtastic_Config_PositionConfig &cfg) override;
    void updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg) override;
    void configCompleted(void) override;

    void packetReceived(const meshtastic_MeshPacket &p) override;
    void newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, bool restore = false) override;
    void restoreMessage(const LogMessage &msg) override;
    void textMessageStatus(uint32_t requestId, LogMessage::MsgStatus status) override;
    void handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route) override;
    void handlePositionResponse(uint32_t from, uint32_t request_id, int32_t rx_rssi, float rx_snr, bool isNeighbor) override;
    void screenSaving(bool enabled) override;

  private:
    friend class ViewFactory;
    static TFieldView *instance(void);
    static TFieldView *instance(const DisplayDriverConfig &cfg);
    TFieldView(const DisplayDriverConfig *cfg, DisplayDriver *driver);

    struct Node {
        std::string shortName, longName;
        uint8_t role = 0;
        int hwModel = 0;
        bool hasKey = false;
        uint8_t channel = 0;
        uint32_t lastHeard = 0; // UTC
        bool hasPos = false;
        double lat = 0, lon = 0;
        int32_t alt = 0;
        uint32_t posTime = 0;
        uint16_t accuracy = 0;
        int8_t battery = -1;
        float voltage = 0;
        float snr = 0;
        int16_t rssi = 0;
        int8_t hops = -1;
    };

    // hooks of the interface
    void connectHooks(void);
    void sendText(const std::string &chatId, const std::string &text, uint32_t msgId, uint32_t sosNode);
    void shareWaypoint(uint32_t to, uint8_t ch, bool pkc, const std::string &name, const std::string &note, double lat,
                       double lon);
    void applySettings(bool preview);
    void saveSettings(void);
    void setRecording(bool on);

    // data for the interface
    void publishStatus(bool force);
    void publishTeam(void);
    void publishOwnPosition(double lat, double lon, int32_t alt, uint32_t epoch, uint16_t acc);
    void pollLocalPosition(void);
    void mergeFirmwareSettings(void);
    std::string chatFor(uint32_t from, uint32_t to, uint8_t ch);
    std::string channelChatId(uint8_t ch) const;
    std::string channelName(uint8_t ch) const;
    int channelOfChat(const std::string &chatId) const;
    std::string nodeName(uint32_t num) const;
    uint32_t nodeColor32(uint32_t num) const;
    uint8_t hopLimitFor(uint32_t num) const;
    std::string localHHMM(uint32_t epoch) const;

    // key input: the keyboard and the trackball go through the interface first
    static void readKeys(lv_indev_t *indev, lv_indev_data_t *data);
    static void readHardware(lv_indev_t *indev, lv_indev_data_t *data);
    void wrapInput(lv_indev_t *indev);
    void pushKey(uint32_t key, bool pressed);

    // files: flash for settings, SD card for tracks and waypoints
    bool readFlash(const char *path, std::string &out);
    bool writeFlash(const char *path, const std::string &content);
    bool sdReady(void);

    static TFieldView *gui;
    TFieldController *ctl;
    tfield::App *app = nullptr;
    IStorage *storage = nullptr;
    bool sdMounted = false;
    std::vector<tfield::TileSource *> layers;

    std::map<uint32_t, Node> nodeDb;
    meshtastic_User ownUser{};
    bool hasOwnUser = false;
    std::string hwName, fwVersion;
    struct Channel {
        bool used = false;
        bool primary = false;
        std::string name;
    };
    Channel channels[c_max_channels];
    meshtastic_Config_LoRaConfig lora{};
    meshtastic_Config_PositionConfig posCfg{};
    meshtastic_Config_DeviceConfig devCfg{};
    bool hasLora = false, hasPosCfg = false, hasDevCfg = false;
    bool uiStarted = false;  // App created
    bool chatsSeeded = false; // channel chats handed to the interface
    int teamChannelIndex = -1;
    tfield::Settings applied; // Meshtastic's part of the settings as the radio has it
    bool haveApplied = false;

    std::map<uint32_t, uint32_t> sentText; // request id -> App message id
    std::map<uint32_t, uint32_t> sosText;  // request id -> node that must confirm the SOS
    bool teamDirty = true;
    uint32_t lastTeam = 0, lastStatusMinute = 0xffffffff, lastPoll = 0;
    int8_t lastBattery = -2;
    uint8_t lastSats = 0xff;
    bool lastFix = false;
    std::string lastName;
    uint32_t localPosTime = 0;
    bool havePos = false;
    double ownLat = 0, ownLon = 0;
    int32_t ownAlt = 0;
    uint8_t ownSats = 0;
    int8_t ownBattery = -1;
    uint32_t savedGpsInterval = 0; // while recording a track

    // keys waiting for the interface's keypad
    struct KeyEvent {
        uint32_t key;
        bool pressed;
    };
    std::vector<KeyEvent> keyQueue;
    uint32_t curKey = 0;
    bool curPressed = false;
    lv_indev_t *keypad = nullptr;
    std::map<lv_indev_t *, lv_indev_read_cb_t> hwRead;
    std::map<lv_indev_t *, std::pair<uint32_t, bool>> hwState;
};
