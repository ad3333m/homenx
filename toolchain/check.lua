function main(toolchain)
    if not is_plat("cross") then
        return false
    end

    local devkitpro = os.getenv("DEVKITPRO") or "/opt/devkitpro"
    if not os.isfile(path.join(devkitpro, "portlibs/switch/include/mbedtls/ctr_drbg.h")) then
        local fallback = path.join(os.getenv("HOME") or "", "Work/devkita64-rootfs/opt/devkitpro")
        if os.isfile(path.join(fallback, "portlibs/switch/include/mbedtls/ctr_drbg.h")) then
            devkitpro = fallback
        end
    end
    local devkita64 = path.join(devkitpro, "devkitA64")
    if not os.isdir(devkita64) then
        return false
    end

    toolchain:config_set("devkita64", devkita64)
    toolchain:config_set("bindir", path.join(devkita64, "bin"))
    return true
end