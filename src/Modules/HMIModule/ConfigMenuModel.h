#pragma once
/**
 * @file ConfigMenuModel.h
 * @brief UI-agnostic configuration menu model with pagination and typed fields.
 */

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

#include "Core/SystemLimits.h"
#include "Core/Services/IConfig.h"

enum class ConfigMenuWidget : uint8_t {
    Text = 0,
    Switch = 1,
    Select = 2,
    Slider = 3
};

enum class ConfigMenuValueType : uint8_t {
    Unknown = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    Text = 4
};

enum class ConfigMenuMode : uint8_t {
    Browse = 0,
    Edit = 1
};

struct ConfigMenuConstraints {
    ConfigMenuWidget widget = ConfigMenuWidget::Text;
    bool editable = true;
    float minValue = 0.0f;
    float maxValue = 100.0f;
    float step = 1.0f;
    const char* optionsCsv = nullptr; // "auto|manual|winter"
};

struct ConfigMenuHint {
    const char* module = nullptr; // exact module name (ex: "poollogic"), nullptr for wildcard
    const char* key = nullptr;    // exact key name
    ConfigMenuConstraints constraints{};
};

struct ConfigMenuRowView {
    bool visible = false;
    bool valueVisible = false;
    bool editable = false;
    bool dirty = false;
    bool canEnter = false;
    bool canEdit = false;
    ConfigMenuWidget widget = ConfigMenuWidget::Text;
    uint8_t editType = 0; // 0=text, 1=int, 2=float, 3=bool
    char key[28]{};
    char label[28]{};
    char value[40]{};
};

struct ConfigMenuView {
    char breadcrumb[96]{};
    uint32_t contextRef = 0U;
    uint8_t pageIndex = 0;
    uint8_t pageCount = 1;
    uint8_t rowCountOnPage = 0;
    ConfigMenuMode mode = ConfigMenuMode::Browse;
    ConfigMenuRowView rows[6]{};

    bool canHome = true;
    bool canBack = false;
    bool canValidate = false;
    bool isHome = true;
};

class ConfigMenuModel {
public:
    static constexpr uint8_t RowsPerPage = 6;
    static constexpr uint8_t MaxRows = 24;
    static constexpr uint8_t MaxModules = Limits::Config::Capacity::ModuleListMax;

    bool begin(const ConfigStoreService* cfgSvc);
    void setHints(const ConfigMenuHint* hints, uint8_t count);

    bool home();
    bool back();
    bool openModule(const char* module);
    bool refreshCurrent();
    bool enterRow(uint8_t rowOnPage);
    bool editRow(uint8_t rowOnPage);

    bool nextPage();
    bool prevPage();

    bool setText(uint8_t rowOnPage, const char* value);
    bool toggleSwitch(uint8_t rowOnPage);
    bool cycleSelect(uint8_t rowOnPage, int8_t direction = 1);
    bool setSlider(uint8_t rowOnPage, float value);

    bool validate(char* ack, size_t ackLen);
    void buildView(ConfigMenuView& out) const;

    bool isHome() const { return currentModule_[0] == '\0'; }
    bool isEditing() const { return mode_ == ConfigMenuMode::Edit; }
    ConfigMenuMode mode() const { return mode_; }
    const char* currentModule() const { return currentModule_; }
    uint8_t pageIndex() const { return pageIndex_; }
    uint8_t pageCount() const;

private:
    struct Row {
        bool editable = false;
        bool hexDisplay = false;
        ConfigMenuWidget widget = ConfigMenuWidget::Text;
        ConfigMenuValueType type = ConfigMenuValueType::Unknown;

        char module[28]{};
        char key[28]{};
        char label[28]{};
        char value[40]{};

        bool boolCur = false;

        int32_t intCur = 0;

        float floatCur = 0.0f;

        char textCur[40]{};

        float sliderMin = 0.0f;
        float sliderMax = 100.0f;
        float sliderStep = 1.0f;

        uint8_t optionCount = 0;
        const char* optionsCsv = nullptr;
    };

    const ConfigStoreService* cfgSvc_ = nullptr;
    const ConfigMenuHint* hints_ = nullptr;
    uint8_t hintCount_ = 0;

    ConfigMenuMode mode_ = ConfigMenuMode::Browse;
    uint8_t pageIndex_ = 0;
    uint8_t editReturnPageIndex_ = 0;

    char currentModule_[28]{};
    char previousModule_[28]{};

    uint8_t listSortedModules_(const char** modules, uint8_t max) const;
    uint8_t currentRowCount_() const;
    uint8_t branchRowCount_(const char* branch) const;
    uint8_t moduleRowCount_(const char* module) const;
    bool branchRowAt_(const char* branch,
                      uint8_t index,
                      char* fullPath,
                      size_t fullPathLen,
                      char* label,
                      size_t labelLen,
                      bool& hasChildren,
                      bool& hasModule,
                      bool& hasAttributes) const;
    bool decorateBranchLabel_(const char* fullPath, const char* child, char* out, size_t outLen) const;
    bool readModuleNameField_(const char* modulePath, const char* key, char* out, size_t outLen) const;
    const char* poolDeviceDisplayNameByRef_(const char* ref) const;
    bool moduleExists_(const char* module) const;
    bool moduleHasChildren_(const char* module) const;
    bool configRowAt_(const char* module, uint8_t index, Row& out) const;
    bool fillRowFromJson_(const char* module, const char* key, JsonVariantConst value, Row& row) const;
    bool applySingleRow_(const Row& row) const;
    bool setRowValueFromText_(Row& row, const char* value) const;
    void buildHomeView_(ConfigMenuView& out) const;
    void buildModuleView_(ConfigMenuView& out) const;
    void formatValueText_(Row& row) const;
    void applyHints_(Row& row) const;
    const ConfigMenuHint* findHint_(const char* module, const char* key) const;
    void buildBreadcrumb_(char* out, size_t outLen) const;
    bool appendText_(char* out, size_t outLen, size_t& pos, const char* text) const;
};
