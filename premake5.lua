workspace "gmsv_gcrash"
	configurations { "Debug", "Debug64", "Release", "Release64" }
	location ( "projects/" .. os.target() )

project "gmsv_gcrash"
	kind         "SharedLib"
	language     "C++"
	includedirs  "include/"
	libdirs      "libs/"
	targetdir    "build"

	if _ACTION == "gmake" then
		postbuildcommands "{MOVE} %{cfg.targetdir}/lib%{prj.name}%{cfg.targetsuffix}.so %{cfg.targetdir}/%{prj.name}%{cfg.targetsuffix}.dll"
	else
		links "lua_shared"
	end

	files
	{
		"src/**.*",
		"include/**.*"
	}

	filter "configurations:Debug"
		architecture "x86"
		optimize "Debug"
		if _ACTION == "gmake" then
			linkoptions "-l:garrysmod/bin/lua_shared_srv.so"
		end
		if os.istarget( "windows" ) then targetsuffix "_win32" end
		if os.istarget( "macosx" )  then targetsuffix "_osx"   end
		if os.istarget( "linux" )   then targetsuffix "_linux" end

	filter "configurations:Debug64"
		architecture "x86_64"
		optimize "Debug"
		if _ACTION == "gmake" then
			linkoptions "-l:bin/linux64/lua_shared.so"
		end
		if os.istarget( "windows" ) then targetsuffix "_win64" end
		if os.istarget( "macosx" )  then targetsuffix "_osx64"   end
		if os.istarget( "linux" )   then targetsuffix "_linux64" end

	filter "configurations:Release"
		architecture "x86"
		optimize "Speed"
		staticruntime "On"
		if _ACTION == "gmake" then
			linkoptions "-l:garrysmod/bin/lua_shared_srv.so"
		end
		if os.istarget( "windows" ) then targetsuffix "_win32" end
		if os.istarget( "macosx" )  then targetsuffix "_osx"   end
		if os.istarget( "linux" )   then targetsuffix "_linux" end
	
	filter "configurations:Release64"
		architecture "x86_64"
		optimize "Speed"
		staticruntime "On"
		if _ACTION == "gmake" then
			linkoptions "-l:bin/linux64/lua_shared.so"
		end
		if os.istarget( "windows" ) then targetsuffix "_win64" end
		if os.istarget( "macosx" )  then targetsuffix "_osx64"   end
		if os.istarget( "linux" )   then targetsuffix "_linux64" end
