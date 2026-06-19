# Upgrading ImGui

1. Create a new branch for the upgrade
2. Checkout latest imgui docking release tag locally
3. Replace all files in iragui/src/imgui with those from the new release
  - Remember to update imgui.natstepfilter imgui.natvis from subfolder
  - Remember to update misc/freetype/imgui_freetype.h and misc/freetype/imgui_freetype.cpp
4. Revert any unwanted changes to imconfig.h e.g. assert and freetype
5. Revert any unwanted changes to source files. Mainly include paths to imgui.h and freetype.h
6. Update ImPlot (if present)
7. Update imgui_markdown (if present)
8. Build application and make any required changes (wrt CHANGELOG.txt)
9. Run application. Check imgui version number in demo window, and test functionality.
10. Look through the CHANGELOG.txt carefully for changes since last upgrade
11. Test the build on Linux (push branch to origin and pull in WSL and run build.sh)
12. Merge the branch into main
