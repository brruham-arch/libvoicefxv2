-- voicechanger.lua v4.0
-- AML mod: libvoicefx.so handle semua hook+DSP
-- Lua hanya controller via dlsym(RTLD_DEFAULT)

local ffi = require("ffi")

ffi.cdef[[
    void* dlsym(void* handle, const char* symbol);
]]

local dl = ffi.load("libdl.so")
local vc = nil

local function loadEngine()
    -- AML sudah load .so ke proses, cari via RTLD_DEFAULT (NULL)
    local ptr = dl.dlsym(nil, "vc_set_pitch")
    if ptr == nil then
        sampAddChatMessage("[VFX] ERROR: libvoicefx.so belum di-load AML", 0xFF4444)
        sampAddChatMessage("[VFX] Pastikan ada di: files/AML/mods/libvoicefx.so", 0xFFFF00)
        return nil
    end

    local function sym(name, sig)
        local p = dl.dlsym(nil, name)
        if p == nil then return nil end
        return ffi.cast(sig, p)
    end

    sampAddChatMessage("[VFX] Engine ditemukan via AML!", 0x00FF88)
    return {
        set_pitch  = sym("vc_set_pitch",  "void(*)(float)"),
        enable     = sym("vc_enable",     "void(*)(void)"),
        disable    = sym("vc_disable",    "void(*)(void)"),
        is_enabled = sym("vc_is_enabled", "int(*)(void)"),
        get_pitch  = sym("vc_get_pitch",  "float(*)(void)"),
    }
end

function main()
    while not isSampAvailable() do wait(100) end
    wait(1000)

    sampAddChatMessage("[VoiceFX] v4.0 loading...", 0xFFFF00)

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
