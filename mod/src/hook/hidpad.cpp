#include "hook/hidpad.h"

#include <Windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/load.h"
#include "core/log.h"

namespace
{
    using gs::hidpad::Family;

    struct Known
    {
        uint16_t vid, pid;
        Family family;
        const char* name;
    };

    // Sony's vendor id and the pads that speak the reports hidpad_parse.cpp
    // reads. The wireless adapter presents itself as a USB DualShock 4.
    constexpr Known kKnown[] = {
        {0x054C, 0x05C4, Family::DualShock4, "DualShock 4"},
        {0x054C, 0x09CC, Family::DualShock4, "DualShock 4"},
        {0x054C, 0x0BA0, Family::DualShock4, "DualShock 4 wireless adapter"},
        {0x054C, 0x0CE6, Family::DualSense, "DualSense"},
        {0x054C, 0x0DF2, Family::DualSense, "DualSense Edge"},
    };

    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;

    std::mutex g_lock;                       // g_handle's lifetime against Rumble
    HANDLE g_handle = INVALID_HANDLE_VALUE;
    bool g_writable = false;
    bool g_bluetooth = false;
    Family g_family = Family::DualSense;
    DWORD g_inLen = 0;
    DWORD g_outLen = 0;

    std::atomic<bool> g_connected{false};
    std::atomic<uint16_t> g_buttons{0};
    std::atomic<uint32_t> g_lastReportMs{0};

    // Find the first known pad and open it. Two tries at access: read and
    // write, for the buzz, then read alone, because something holding the pad
    // can refuse a writer and a pad that cannot buzz still has buttons.
    bool Open()
    {
        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return false;

        bool opened = false;
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        for (DWORD i = 0; !opened && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i)
        {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
            if (!need) continue;
            std::vector<BYTE> buf(need);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            detail->cbSize = sizeof(*detail);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;

            // A handle with no access is enough to ask who the device is, and
            // it opens even for devices something else holds exclusively.
            HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES attr{};
            attr.Size = sizeof(attr);
            const bool haveAttr = HidD_GetAttributes(probe, &attr) != FALSE;
            CloseHandle(probe);
            if (!haveAttr) continue;

            const Known* known = nullptr;
            for (const Known& k : kKnown)
                if (k.vid == attr.VendorID && k.pid == attr.ProductID) { known = &k; break; }
            if (!known) continue;

            bool writable = true;
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                writable = false;
                h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE)
            {
                GS_LOG("pad: found a %s but could not open it for reading (error %lu); "
                       "something may be holding it exclusively", known->name, GetLastError());
                continue;
            }

            PHIDP_PREPARSED_DATA pp = nullptr;
            HIDP_CAPS caps{};
            if (!HidD_GetPreparsedData(h, &pp) || HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS)
            {
                if (pp) HidD_FreePreparsedData(pp);
                CloseHandle(h);
                continue;
            }
            HidD_FreePreparsedData(pp);

            // Both families report 64 bytes over USB. Over Bluetooth the
            // descriptor declares longer reports, 78 for a DualSense and more
            // for a DualShock 4, so anything else is the wireless link.
            const bool bluetooth = caps.InputReportByteLength != 64;

            {
                std::lock_guard<std::mutex> hold(g_lock);
                g_handle = h;
                g_writable = writable;
                g_bluetooth = bluetooth;
                g_family = known->family;
                g_inLen = caps.InputReportByteLength;
                g_outLen = caps.OutputReportByteLength;
            }
            opened = true;
            const bool canBuzz = writable && !bluetooth;
            GS_LOG("pad: %s over %s, read directly (VID %04X PID %04X). %s", known->name,
                   bluetooth ? "Bluetooth" : "USB", attr.VendorID, attr.ProductID,
                   canBuzz ? "The chord works and it buzzes when a pin lands."
                   : bluetooth ? "The chord works. No buzz over Bluetooth yet."
                               : "The chord works. It could not be opened for writing, so no buzz.");
        }
        SetupDiDestroyDeviceInfoList(set);
        return opened;
    }

    void Close(const char* why)
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_handle == INVALID_HANDLE_VALUE) return;
        CancelIoEx(g_handle, nullptr);
        CloseHandle(g_handle);
        g_handle = INVALID_HANDLE_VALUE;
        g_connected.store(false);
        g_buttons.store(0);
        if (why) GS_LOG("pad: the directly read pad is gone (%s)", why);
    }

    DWORD WINAPI Reader(LPVOID)
    {
        std::vector<uint8_t> report;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return 0;
        uint32_t nextLookMs = 0;
        bool saidNone = false;

        while (!g_stop.load())
        {
            HANDLE h;
            bool bluetooth;
            Family family;
            DWORD inLen;
            {
                std::lock_guard<std::mutex> hold(g_lock);
                h = g_handle;
                bluetooth = g_bluetooth;
                family = g_family;
                inLen = g_inLen;
            }

            if (h == INVALID_HANDLE_VALUE)
            {
                const uint32_t now = GetTickCount();
                if (static_cast<int32_t>(now - nextLookMs) >= 0)
                {
                    nextLookMs = now + 2000;
                    if (Open())
                    {
                        saidNone = false;
                        g_lastReportMs.store(now ? now : 1);
                        continue;
                    }
                    if (!saidNone)
                    {
                        saidNone = true;
                        GS_LOG("pad: no DualSense or DualShock 4 to read directly; still looking every two seconds");
                    }
                }
                Sleep(250);
                continue;
            }

            if (report.size() < inLen) report.resize(inLen);
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            BOOL ok = ReadFile(h, report.data(), inLen, &got, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                // A quarter second at most, so a stop request is heard. Both
                // pads report every few milliseconds while plugged in, so a
                // timeout here is a pad that has gone quiet.
                if (WaitForSingleObject(ov.hEvent, 250) != WAIT_OBJECT_0)
                {
                    CancelIoEx(h, &ov);
                    GetOverlappedResult(h, &ov, &got, TRUE);
                    continue;
                }
                ok = GetOverlappedResult(h, &ov, &got, FALSE);
            }
            if (!ok)
            {
                const DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED && g_stop.load()) break;
                char why[64];
                snprintf(why, sizeof(why), "read failed, error %lu", err);
                Close(why);
                continue;
            }

            uint16_t buttons = 0;
            if (gs::hidpad::ParseInput(family, bluetooth, report.data(), got, &buttons))
            {
                g_buttons.store(buttons);
                const uint32_t now = GetTickCount();
                g_lastReportMs.store(now ? now : 1);
                g_connected.store(true);
            }
        }

        Close(nullptr);
        CloseHandle(ov.hEvent);
        return 0;
    }
}

namespace gs::hidpad
{
    void Start()
    {
        if (g_thread) return;
        g_stop.store(false);
        g_thread = CreateThread(nullptr, 0, Reader, nullptr, 0, nullptr);
        gs::load::AddThread("pad reader", g_thread);
        if (!g_thread) GS_LOG_ERR("pad: could not start the direct reader, error %lu", GetLastError());
    }

    void Stop(bool processTerminating)
    {
        if (!g_thread) return;
        g_stop.store(true);
        // The same rule as the other threads: never wait on one during process
        // teardown, where the loader lock is held and it may already be gone.
        if (!processTerminating) WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    bool Connected() { return g_connected.load(); }

    uint16_t Buttons()
    {
        if (!g_connected.load()) return 0;
        // A pad that stopped reporting holds its last buttons for ever, and a
        // chord frozen down fires the moment the hold time passes. A second of
        // silence reads as nothing held.
        const uint32_t last = g_lastReportMs.load();
        if (!last || GetTickCount() - last > 1000) return 0;
        return g_buttons.load();
    }

    bool CanRumble()
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return g_handle != INVALID_HANDLE_VALUE && g_writable && !g_bluetooth;
    }

    bool Rumble(uint8_t strength)
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_handle == INVALID_HANDLE_VALUE || !g_writable) return false;
        uint8_t buf[80]{};
        size_t len = BuildRumble(g_family, g_bluetooth, strength, buf, sizeof(buf));
        if (!len) return false;
        // The driver wants the device's full output length, which can be
        // longer than the report itself; the tail stays zero.
        if (g_outLen > len && g_outLen <= sizeof(buf)) len = g_outLen;

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD wrote = 0;
        BOOL ok = WriteFile(g_handle, buf, static_cast<DWORD>(len), &wrote, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(g_handle, &ov, &wrote, FALSE);
            else
            {
                CancelIoEx(g_handle, &ov);
                GetOverlappedResult(g_handle, &ov, &wrote, TRUE);
            }
        }
        CloseHandle(ov.hEvent);
        return ok != FALSE;
    }
}
