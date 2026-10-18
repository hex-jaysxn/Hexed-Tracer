script_name('Hexed Tracer')
script_author('HEXED Studio')
script_version('1.0')

-- Menu for the Hexed bullet tracer (libHexedTracer.so).
-- Textures: put PNG files in  Android/data/<game package>/files/HexedTracer/
-- Open with /hexedtracer.  Settings are saved to HexedTracer.ini.

local imgui  = require 'mimgui'
local ffi    = require 'ffi'
local inicfg = require 'inicfg'

local new = imgui.new

local ini = inicfg.load({
    main = { enabled = true, thick = 1.0, fade = 0.6, tile = 0.0, glow = false,
             usecolor = false, r = 1.0, g = 1.0, b = 1.0, a = 1.0,
             tex = 'hxd_custom.png' }
}, 'HexedTracer.ini')
inicfg.save(ini, 'HexedTracer.ini')

local window   = new.bool(false)
local enabled  = new.bool(ini.main.enabled)
local useColor = new.bool(ini.main.usecolor)
local glow     = new.bool(ini.main.glow)
local thick    = new.float(ini.main.thick)
local fade     = new.float(ini.main.fade)
local tile     = new.float(ini.main.tile)
local col      = new.float[4](ini.main.r, ini.main.g, ini.main.b, ini.main.a)
local texName  = tostring(ini.main.tex or 'hxd_custom.png')

-- ---------------------------------------------------------------- native link
-- HexedTracerCfg (float[16]): 0 enabled, 1 thick, 2 fade, 3-6 rgba, 7 texture index,
-- 8 glow, 9 tile, 10 status, 11 active traces, 12 texture count, 13 list generation,
-- 14 rescan request, 15 Im3D flags override
ffi.cdef[[
    void* dlopen(const char* name, int flags);
    void* dlsym(void* handle, const char* symbol);
]]

local cfgPtr, getName, getDir = nil, nil, nil
local texList, lastGen = {}, -1

local function linkNative()
    if cfgPtr ~= nil then return true end
    -- already-loaded lib by name (RTLD_NOLOAD | RTLD_NOW = 4 | 2)
    local ok, h = pcall(function() return ffi.C.dlopen('libHexedTracer.so', 6) end)
    if ok and h ~= nil then
        local sym = ffi.C.dlsym(h, 'HexedTracerCfg')
        if sym ~= nil then
            cfgPtr  = ffi.cast('volatile float*', sym)
            local g1 = ffi.C.dlsym(h, 'HexedTracerGetTextureName')
            local g2 = ffi.C.dlsym(h, 'HexedTracerGetDir')
            if g1 ~= nil then getName = ffi.cast('const char*(*)(int)', g1) end
            if g2 ~= nil then getDir  = ffi.cast('const char*(*)()', g2) end
            return true
        end
    end
    return false
end

local function readList()
    texList = {}
    if cfgPtr == nil or getName == nil then return end
    local n = math.floor(cfgPtr[12] + 0.5)
    for i = 0, n - 1 do
        local p = getName(i)
        if p ~= nil then texList[#texList + 1] = ffi.string(p) end
    end
    lastGen = cfgPtr[13]
end

local function texIndex()
    for i, name in ipairs(texList) do
        if name == texName then return i - 1 end
    end
    return 0
end

local function apply()
    if not linkNative() then return end
    if lastGen ~= cfgPtr[13] then readList() end
    cfgPtr[0] = enabled[0] and 1 or 0
    cfgPtr[1] = thick[0]
    cfgPtr[2] = fade[0]
    if useColor[0] then
        cfgPtr[3], cfgPtr[4], cfgPtr[5], cfgPtr[6] = col[0], col[1], col[2], col[3]
    else
        cfgPtr[3], cfgPtr[4], cfgPtr[5], cfgPtr[6] = 1, 1, 1, 1
    end
    cfgPtr[7] = texIndex()
    cfgPtr[8] = glow[0] and 1 or 0
    cfgPtr[9] = tile[0]
end

local function save()
    ini.main.enabled  = enabled[0]
    ini.main.usecolor = useColor[0]
    ini.main.glow     = glow[0]
    ini.main.thick    = thick[0]
    ini.main.fade     = fade[0]
    ini.main.tile     = tile[0]
    ini.main.tex      = texName
    ini.main.r, ini.main.g, ini.main.b, ini.main.a = col[0], col[1], col[2], col[3]
    inicfg.save(ini, 'HexedTracer.ini')
end

local function setColor(r, g, b)
    col[0], col[1], col[2] = r, g, b
    useColor[0] = true
    apply() save()
end

local function hasTex(name)
    for _, n in ipairs(texList) do if n == name then return true end end
    return false
end

local function texButton(label, file)
    local active = texName == file
    if active then
        imgui.PushStyleColor(imgui.Col.Button, imgui.GetStyle().Colors[imgui.Col.ButtonActive])
    end
    local clicked = imgui.Button(label)
    if active then imgui.PopStyleColor() end
    if clicked then texName = file end
    return clicked
end

-- ---------------------------------------------------------------- menu
imgui.OnFrame(function() return window[0] end, function()
    imgui.SetNextWindowSize(imgui.ImVec2(460, 0), imgui.Cond.FirstUseEver)
    imgui.Begin('Hexed Tracer - HEXED Studio', window, imgui.WindowFlags.AlwaysAutoResize)

    local changed = false
    if imgui.Checkbox('Enabled', enabled) then changed = true end

    imgui.Separator()
    imgui.Text('Style')
    if hasTex('hxd_vctraces.png') then
        if texButton('VC Traces', 'hxd_vctraces.png') then changed = true end imgui.SameLine()
    end
    if hasTex('hxd_solid.png') then
        if texButton('Solid', 'hxd_solid.png') then changed = true end imgui.SameLine()
    end
    if hasTex('hxd_custom.png') then
        if texButton('Custom', 'hxd_custom.png') then changed = true end
    end

    if imgui.CollapsingHeader('All textures (' .. #texList .. ')###alltex') then
        for _, name in ipairs(texList) do
            if imgui.Selectable(name, name == texName) then texName = name changed = true end
        end
        if imgui.Button('Refresh list') and cfgPtr ~= nil then cfgPtr[14] = 1 end
    end

    imgui.Separator()
    imgui.Text('Thickness')
    imgui.PushItemWidth(-1)
    if imgui.SliderFloat('##thick', thick, 0.2, 6.0, '%.2fx') then changed = true end
    imgui.PopItemWidth()

    imgui.Text('Fade duration')
    imgui.PushItemWidth(-1)
    if imgui.SliderFloat('##fade', fade, 0.05, 3.0, '%.2f s') then changed = true end
    imgui.PopItemWidth()

    imgui.Text('Texture tiling (0 = stretch)')
    imgui.PushItemWidth(-1)
    if imgui.SliderFloat('##tile', tile, 0.0, 20.0, '%.1f m') then changed = true end
    imgui.PopItemWidth()

    if imgui.Checkbox('Glow (additive)', glow) then changed = true end

    imgui.Separator()
    if imgui.Checkbox('Custom color', useColor) then changed = true end
    imgui.PushItemWidth(-1)
    if imgui.ColorEdit4('##col', col) then useColor[0] = true changed = true end
    imgui.PopItemWidth()

    if imgui.Button('White')  then setColor(1, 1, 1) end imgui.SameLine()
    if imgui.Button('Red')    then setColor(1, 0.1, 0.1) end imgui.SameLine()
    if imgui.Button('Green')  then setColor(0.1, 1, 0.2) end imgui.SameLine()
    if imgui.Button('Blue')   then setColor(0.2, 0.5, 1) end imgui.SameLine()
    if imgui.Button('Yellow') then setColor(1, 0.9, 0.1) end

    imgui.Separator()
    if imgui.Button('Reset to default') then
        enabled[0], useColor[0], glow[0] = true, false, false
        thick[0], fade[0], tile[0] = 1.0, 0.6, 0.0
        col[0], col[1], col[2], col[3] = 1, 1, 1, 1
        if hasTex('hxd_custom.png') then texName = 'hxd_custom.png' end
        changed = true
    end

    if changed then apply() save() end

    -- status line (helps when something isn't hooked)
    imgui.Separator()
    if cfgPtr ~= nil then
        local st = math.floor(cfgPtr[10] + 0.5)
        imgui.TextDisabled(('Mod linked | hooks: Add:%s%s Render:%s | texture: %s | drawn: %s'):format(
            (st % 2 >= 1) and 'OK' or 'NO',
            (st % 4 >= 2) and '+OK' or '+NO',
            (st % 8 >= 4) and 'OK' or 'NO',
            (st % 32 >= 16) and 'ready' or 'NOT loaded',
            (st % 64 >= 32) and 'yes' or 'not yet'))
        imgui.TextDisabled(('active: %d | camera: %s'):format(
            math.floor(cfgPtr[11] + 0.5), (st % 16 >= 8) and 'ok' or 'fallback'))
        if #texList == 0 then
            local dir = (getDir ~= nil) and ffi.string(getDir()) or 'HexedTracer folder'
            imgui.TextDisabled('No PNG found in: ' .. dir)
        end
    else
        imgui.TextDisabled('libHexedTracer.so not loaded')
    end

    imgui.End()
end)

-- ---------------------------------------------------------------- main
function main()
    while not isSampAvailable() do wait(100) end
    sampRegisterChatCommand('hexedtracer', function() window[0] = not window[0] end)
    -- debug: /hxdflags <n>  overrides the Im3D vertex flags (0 = default)
    sampRegisterChatCommand('hxdflags', function(arg)
        if cfgPtr ~= nil then cfgPtr[15] = tonumber(arg) or 0 end
    end)
    while true do
        wait(500)
        apply()   -- keeps settings applied even if the mod loads late
    end
end
