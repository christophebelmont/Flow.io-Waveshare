#include "Modules/HMIModule/Drivers/NextionDriver.h"

#include <cassert>
#include <initializer_list>

uint32_t testNowMs = 0;

struct Fixture {
    HardwareSerial serial;
    NextionDriver driver;

    Fixture() {
        NextionDriverConfig config{};
        config.serial = &serial;
        config.homePageAliasId = 1U;
        config.configPageAliasId = 2U;
        config.alarmPageAliasId = 3U;
        driver.setConfig(config);
        assert(driver.begin());
        serial.transmitted.clear();
    }

    HmiEvent receive(std::initializer_list<uint8_t> bytes) {
        serial.received.insert(serial.received.end(), bytes);
        HmiEvent event{};
        assert(driver.pollEvent(event));
        assert(serial.received.empty());
        return event;
    }

    HmiEvent page(uint8_t id) { return receive({'#', 2, 0x50, id}); }

    HmiEvent contextualPage(uint8_t id) {
        return receive({'#', 6, 0x50, id, 0x78, 0x56, 0x34, 0x12});
    }

    void pumpCommand() {
        const uint32_t before = millis();
        const HmiEvent event = receive({'#', 3, 0x60, 9, 1});
        assert(event.type == HmiEventType::Command);
        assert(event.command == HmiCommandId::HomeFiltrationToggle);
        assert(millis() == before);
    }
};

void test_home_after_explicit_menu_exit()
{
    Fixture f;
    assert(f.receive({0x66, 0, 0xFF, 0xFF, 0xFF}).type == HmiEventType::Home);
    f.pumpCommand();

    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        assert(f.page(10).type == HmiEventType::ConfigEnter);
        // bHome release and the page's unload event both announce the exit.
        for (unsigned exit = 0; exit < 2; ++exit) {
            const HmiEvent event = f.receive({'#', 2, 0x51, 6});
            assert(event.type == HmiEventType::ConfigExit);
            assert(event.pageId == 0xFFU);
        }
        const HmiEvent home = f.page(0);
        assert(home.type == HmiEventType::Home);
        assert(home.pageId == 0U);
        assert(f.driver.isHomePage());
        assert(!f.driver.isConfigPage());
        f.pumpCommand();
    }
}

void test_page_reports_preserve_navigation_types()
{
    Fixture f;
    assert(f.page(10).type == HmiEventType::ConfigEnter);
    const HmiEvent exit = f.page(0);
    assert(exit.type == HmiEventType::ConfigExit);
    assert(exit.pageId == 0U);
    assert(f.driver.isHomePage());

    const HmiEvent keyboard = f.page(12);
    assert(keyboard.type == HmiEventType::Page);
    assert(keyboard.pageId == 12U);
    assert(f.page(0).type == HmiEventType::Home);

    // A duplicate report does not manufacture another navigation event.
    f.serial.received = {'#', 2, 0x50, 0};
    HmiEvent event{};
    assert(!f.driver.pollEvent(event));
}

void test_context_is_restored_only_on_menu_pages()
{
    Fixture f;
    for (uint8_t page : {10, 2, 11, 3}) {
        // Context also resumes a menu that is already the current page.
        assert(f.page(page).type == HmiEventType::ConfigEnter);
        const HmiEvent resume = f.contextualPage(page);
        assert(resume.type == HmiEventType::ConfigEnter);
        assert(resume.pageId == page);
        assert(resume.contextRef == 0x12345678U);
        const HmiEvent exit = f.page(0);
        assert(exit.type == HmiEventType::ConfigExit);
        assert(exit.pageId == 0U);
    }
    const HmiEvent other = f.contextualPage(12);
    assert(other.type == HmiEventType::Page);
    assert(other.contextRef == 0U);
    const HmiEvent home = f.contextualPage(1);
    assert(home.type == HmiEventType::Home);
    assert(home.contextRef == 0U);
}

void test_native_and_custom_reports_agree()
{
    Fixture f;
    assert(f.receive({0x66, 0, 0xFF, 0xFF, 0xFF}).type == HmiEventType::Home);
    assert(f.page(2).type == HmiEventType::ConfigEnter);
    const HmiEvent exit = f.receive({0x66, 1, 0xFF, 0xFF, 0xFF});
    assert(exit.type == HmiEventType::ConfigExit);
    assert(exit.pageId == 1U);
    assert(f.page(12).type == HmiEventType::Page);
    assert(f.page(1).type == HmiEventType::Home);
}

int main()
{
    test_home_after_explicit_menu_exit();
    test_page_reports_preserve_navigation_types();
    test_context_is_restored_only_on_menu_pages();
    test_native_and_custom_reports_agree();
}
