package("spdlog_radray")
    set_base("spdlog")

    add_patches("v1.15.0", "patches/v1.15.0/0001-fix-remove-unused-to_string_view-overload-in-fmt-11..patch", "e35f3c6754ede56670603f6e888758f10269987deba6e225cf5b807a5e15837c")

    add_configs("no_thread_id", {description = "prevent spdlog from querying the thread id on each log call if thread id is not needed", default = false, type = "boolean"})
    add_configs("no_default_logger", {description = "Disable default logger creation", default = false, type = "boolean"})

    on_load(function (package)
        if package:config("header_only") then
            package:set("kind", "library", {headeronly = true})
        else
            package:add("defines", "SPDLOG_COMPILED_LIB")
            package:add("deps", "cmake")
        end
        assert(not (package:config("fmt_external") and package:config("fmt_external_ho")), "fmt_external and fmt_external_ho are mutually exclusive")
        if package:config("std_format") then
            package:add("defines", "SPDLOG_USE_STD_FORMAT")
        elseif package:config("fmt_external") or package:config("fmt_external_ho") then
            package:add("defines", "SPDLOG_FMT_EXTERNAL")
            package:add("deps", "fmt", {configs = {header_only = package:config("header_only")}})
        end
        if not package:config("header_only") and package:config("fmt_external_ho") then
            package:add("defines", "FMT_HEADER_ONLY=1")
        end
        if package:config("noexcept") then
            package:add("defines", "SPDLOG_NO_EXCEPTIONS")
        end
        if package:config("wchar") then
            package:add("defines", "SPDLOG_WCHAR_TO_UTF8_SUPPORT")
        end
        if package:config("no_thread_id") then
            package:add("defines", "SPDLOG_NO_THREAD_ID")
        end
        if package:config("no_default_logger") then
            package:add("defines", "SPDLOG_DISABLE_DEFAULT_LOGGER")
        end
    end)

    on_install(function (package)
        if (not package:gitref() and package:version():lt("1.4.0")) or package:config("header_only") then
            os.cp("include", package:installdir())
            return
        end

        local configs = {"-DSPDLOG_BUILD_TESTS=OFF", "-DSPDLOG_BUILD_EXAMPLE=OFF"}
        table.insert(configs, "-DSPDLOG_BUILD_SHARED=" .. (package:config("shared") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_USE_STD_FORMAT=" .. (package:config("std_format") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_FMT_EXTERNAL=" .. (package:config("fmt_external") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_FMT_EXTERNAL_HO=" .. (package:config("fmt_external_ho") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_NO_EXCEPTIONS=" .. (package:config("noexcept") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_WCHAR_SUPPORT=" .. (package:config("wchar") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_NO_THREAD_ID=" .. (package:config("no_thread_id") and "ON" or "OFF"))
        table.insert(configs, "-DSPDLOG_DISABLE_DEFAULT_LOGGER=" .. (package:config("no_default_logger") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs, {packages = "fmt"})
    end)
