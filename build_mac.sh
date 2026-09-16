cmake -G Xcode -S . -B ./temp/cmake -Dgearmulator_BUILD_JUCEPLUGIN_Standalone=ON -Dgearmulator_BUILD_JUCEPLUGIN=ON -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -Dgearmulator_BUILD_JUCEPLUGIN_LV2=OFF -Dgearmulator_SYNTH_OSIRUS=OFF -Dgearmulator_SYNTH_OSTIRUS=OFF -Dgearmulator_SYNTH_VAVRA=OFF -Dgearmulator_SYNTH_XENIA=OFF -Dgearmulator_SYNTH_NODALRED2X=OFF
cd ./temp/cmake
cmake --build . --config Release
cpack -G ZIP
