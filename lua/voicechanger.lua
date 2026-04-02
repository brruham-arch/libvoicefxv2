-- voicechanger.lua v4.2
-- Baca alamat vc_api dari file yang ditulis OnModLoad
-- Lalu cast ke struct dan panggil langsung

local ffi = require("ffi")

ffi.cdef[[
    typedef struct {
        void  (*set_pitch)(float);
        void  (*enable)(void);
        void  (*disable)(void);
        int   (*is_enabled)(void);
        float (*get_pitch)(void);
    } VcAPI;
]]

local vc = nil

local function loadEngine()
    -- Baca alamat vc_api yang ditulis OnModLoad ke file
    local f = io.open("/storage/emulated/0/voicefx_addr.txt", "r")
    if not f then
        sampAddChatMessage("[VFX] voicefx_addr.txt tidak ditemukan!", 0xFF4444)
        sampAddChatMessage("[VFX] Pastikan libvoicefx.so ada di AML/mods/", 0xFFFF00)
        return nil
    end

    local addrStr = f:read("*l")
    f:close()

    local addr = tonumber(addrStr)
    if not addr or addr == 0 then
        sampAddChatMessage("[VFX] Alamat tidak valid: " .. tostring(addrStr), 0xFF4444)
        return nil
    end

    sampAddChatMessage("[VFX] vc_api addr: 0x" .. string.format("%x", addr), 0x00FFFF)

    -- Cast alamat ke pointer struct VcAPI
    local api = ffi.cast("VcAPI*", addr)

    -- Verifikasi fungsi tidak null
    if api.set_pitch == nil then
        sampAddChatMessage("[VFX] set_pitch null!", 0xFF4444)
        return nil
    end

    sampAddChatMessage("[VFX] Engine loaded!", 0x00FF88)
    return api
end

function main()
    while not isSampAvailable() do wait(100) end
    wait(1000)

    sampAddChatMessage("[VoiceFX] v4.2 loading...", 0xFFFF00)

    vc = loadEngine()
    if vc == nil then return end

    sampAddChatMessage("[VFX] SIAP! /vfx=toggle | /vfp [0.3-3.0]=pitch | /vfs=status", 0x00FFFF)

    sampRegisterChatCommand("vfx", function()
        if vc == nil then return end
        if vc.is_enabled() == 0 then
            vc.enable()
            sampAddChatMessage("[VFX] ON pitch=" .. vc.get_pitch(), 0x00FF88)
        else
            vc.disable()
            sampAddChatMessage("[VFX] OFF", 0xFF8800)
        end
    end)

    sampRegisterChatCommand("vfp", function(arg)
        if vc == nil then return end
        local v = tonumber(arg)
        if v and v >= 0.3 and v <= 3.0 then
            vc.set_pitch(v)
            local desc = v > 1.0 and "tinggi" or v < 1.0 and "rendah" or "normal"
            sampAddChatMessage("[VFX] Pitch=" .. v .. " (" .. desc .. ")", 0x00FFFF)
        else
            sampAddChatMessage("[VFX] /vfp [0.3-3.0] sekarang=" .. vc.get_pitch(), 0xFFFF00)
        end
    end)

    sampRegisterChatCommand("vfs", function()
        if vc == nil then
            sampAddChatMessage("[VFX] Engine tidak loaded", 0xFF4444)
            return
        end
        local st = vc.is_enabled() == 1 and "ON" or "OFF"
        sampAddChatMessage("[VFX] " .. st .. " | pitch=" .. vc.get_pitch(), 0x00FFFF)
    end)

    while true do wait(1000) end
end
