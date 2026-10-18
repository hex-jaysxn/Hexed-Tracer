HEXED TRACER  -  install guide
================================

1) BUILD libHexedTracer.so (GitHub does it for you)
   - Upload these files to your repo (keep the folders):
       HexedTracer.cpp, build.sh, .github/workflows/build.yml
     (phone can't upload the hidden .github folder? use "Add file > Create new file",
      name it  .github/workflows/build.yml  and paste the contents)
   - Open the Actions tab -> wait for the green check -> download artifact "libHexedTracer"
     -> unzip -> libHexedTracer.so

2) INSTALL on the phone
   a) AML mods folder:  put  libHexedTracer.so
   b) DELETE the old tracer libs from the mods folder, otherwise two tracers fight:
        libVC_Tracer_Nezuko.so   libVCTracerMenu.so   libVCTracerSolid.so
   c) Create / copy the texture folder (the mod also tries to create it by itself):
        Android/data/com.nezukosamp.game/files/HexedTracer/
      and copy the 3 PNGs from the "HexedTracer" folder of this kit into it:
        hxd_custom.png   hxd_solid.png   hxd_vctraces.png
      (these are the same pictures you sent, resized to power-of-two so the game can load them;
       any extra .png you drop in that folder shows up in the menu -> "Refresh list")
   d) Lua: put HexedTracer.lua in your MonetLoader scripts folder (remove VC_Tracer_Menu.lua).

3) USE
   Start the game, type  /hexedtracer
   Styles: VC Traces / Solid / Custom (lightning).  Sliders: Thickness, Fade duration, Texture tiling.
   Glow (additive), Custom color (RGBA).  Status lines at the bottom tell you what is hooked.

If something is off, send me the two status lines from the bottom of the menu.
Debug: /hxdflags <n> overrides the Im3D vertex flag (default 1) - only if tracers are invisible.
