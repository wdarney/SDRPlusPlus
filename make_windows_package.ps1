$build_dir=$args[0]
$root_dir=$args[1]

function Copy-Optional($source, $destination) {
    if (Test-Path $source) {
        Copy-Item $source $destination
    }
    else {
        Write-Host "Skipping optional file: $source"
    }
}

Remove-Item -Force -Recurse sdrpp_windows_x64 -ErrorAction SilentlyContinue
Remove-Item -Force sdrpp_windows_x64.zip -ErrorAction SilentlyContinue
mkdir sdrpp_windows_x64

# Copy root
cp -Recurse $root_dir/* sdrpp_windows_x64/

# Copy core
cp $build_dir/Release/* sdrpp_windows_x64/
cp 'C:/Program Files/PothosSDR/bin/volk.dll' sdrpp_windows_x64/

# Copy source modules
cp $build_dir/source_modules/airspy_source/Release/airspy_source.dll sdrpp_windows_x64/modules/
cp 'C:/Program Files/PothosSDR/bin/airspy.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/airspyhf_source/Release/airspyhf_source.dll sdrpp_windows_x64/modules/
cp 'C:/Program Files/PothosSDR/bin/airspyhf.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/audio_source/Release/audio_source.dll sdrpp_windows_x64/modules/

Copy-Optional $build_dir/source_modules/bladerf_source/Release/bladerf_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/PothosSDR/bin/bladeRF.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/file_source/Release/file_source.dll sdrpp_windows_x64/modules/

Copy-Optional $build_dir/source_modules/fobossdr_source/Release/fobossdr_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/RigExpert/Fobos/bin/fobos.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/hackrf_source/Release/hackrf_source.dll sdrpp_windows_x64/modules/
cp 'C:/Program Files/PothosSDR/bin/hackrf.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/hermes_source/Release/hermes_source.dll sdrpp_windows_x64/modules/

Copy-Optional $build_dir/source_modules/hydrasdr_source/Release/hydrasdr_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/hydrasdr-host/bin/hydrasdr.dll' sdrpp_windows_x64/

Copy-Optional $build_dir/source_modules/limesdr_source/Release/limesdr_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/PothosSDR/bin/LimeSuite.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/network_source/Release/network_source.dll sdrpp_windows_x64/modules/

Copy-Optional $build_dir/source_modules/perseus_source/Release/perseus_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/PothosSDR/bin/perseus-sdr.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/plutosdr_source/Release/plutosdr_source.dll sdrpp_windows_x64/modules/
cp 'C:/Program Files/PothosSDR/bin/libiio.dll' sdrpp_windows_x64/
cp 'C:/Program Files/PothosSDR/bin/libad9361.dll' sdrpp_windows_x64/

Copy-Optional $build_dir/source_modules/rfnm_source/Release/rfnm_source.dll sdrpp_windows_x64/modules/
Copy-Optional 'C:/Program Files/RFNM/bin/rfnm.dll' sdrpp_windows_x64/
Copy-Optional 'C:/Program Files/RFNM/bin/spdlog.dll' sdrpp_windows_x64/
Copy-Optional 'C:/Program Files/RFNM/bin/fmt.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/rfspace_source/Release/rfspace_source.dll sdrpp_windows_x64/modules/

cp $build_dir/source_modules/rtl_sdr_source/Release/rtl_sdr_source.dll sdrpp_windows_x64/modules/
cp 'C:/Program Files/PothosSDR/bin/rtlsdr.dll' sdrpp_windows_x64/

cp $build_dir/source_modules/rtl_tcp_source/Release/rtl_tcp_source.dll sdrpp_windows_x64/modules/

cp $build_dir/source_modules/sdrplay_source/Release/sdrplay_source.dll sdrpp_windows_x64/modules/ -ErrorAction SilentlyContinue
cp 'C:/Program Files/SDRplay/API/x64/sdrplay_api.dll' sdrpp_windows_x64/ -ErrorAction SilentlyContinue

cp $build_dir/source_modules/sdrpp_server_source/Release/sdrpp_server_source.dll sdrpp_windows_x64/modules/

cp $build_dir/source_modules/spyserver_source/Release/spyserver_source.dll sdrpp_windows_x64/modules/

cp $build_dir/source_modules/spectran_http_source/Release/spectran_http_source.dll sdrpp_windows_x64/modules/

cp $build_dir/source_modules/rx888_source/Release/rx888_source.dll sdrpp_windows_x64/modules/ -ErrorAction SilentlyContinue
cp 'C:/Program Files/PothosSDR/bin/SoapySDR.dll' sdrpp_windows_x64/
mkdir sdrpp_windows_x64/SoapySDR
mkdir sdrpp_windows_x64/SoapySDR/modules0.8
cp 'C:/Program Files/PothosSDR/lib/SoapySDR/modules0.8/SDDCSupport.dll' sdrpp_windows_x64/SoapySDR/modules0.8/

cp $build_dir/source_modules/sddc_source/Release/sddc_source.dll sdrpp_windows_x64/modules/ -ErrorAction SilentlyContinue

# cp $build_dir/source_modules/usrp_source/Release/usrp_source.dll sdrpp_windows_x64/modules/


# Copy sink modules
cp $build_dir/sink_modules/audio_sink/Release/audio_sink.dll sdrpp_windows_x64/modules/
cp "C:/Program Files (x86)/RtAudio/bin/rtaudio.dll" sdrpp_windows_x64/

cp $build_dir/sink_modules/network_sink/Release/network_sink.dll sdrpp_windows_x64/modules/


# Copy decoder modules
cp $build_dir/decoder_modules/atv_decoder/Release/atv_decoder.dll sdrpp_windows_x64/modules/

cp $build_dir/decoder_modules/ch_extravhf_decoder/Release/ch_extravhf_decoder.dll sdrpp_windows_x64/modules/
cp $build_dir/mbelib-build/Release/mbe-shared.dll sdrpp_windows_x64/
cp "C:/vcpkg/installed/x64-windows/bin/itpp.dll" sdrpp_windows_x64/

Copy-Optional $build_dir/decoder_modules/m17_decoder/Release/m17_decoder.dll sdrpp_windows_x64/modules/
Copy-Optional "C:/Program Files/codec2/lib/libcodec2.dll" sdrpp_windows_x64/

cp $build_dir/decoder_modules/meteor_demodulator/Release/meteor_demodulator.dll sdrpp_windows_x64/modules/

cp $build_dir/decoder_modules/pager_decoder/Release/pager_decoder.dll sdrpp_windows_x64/modules/

cp $build_dir/decoder_modules/radio/Release/radio.dll sdrpp_windows_x64/modules/

cp $build_dir/decoder_modules/selcal_decoder/Release/selcal_decoder.dll sdrpp_windows_x64/modules/

cp $build_dir/decoder_modules/vdl2_decoder/Release/vdl2_decoder.dll sdrpp_windows_x64/modules/
cp $build_dir/decoder_modules/vdl2_decoder/Release/sqlite3.dll sdrpp_windows_x64/


# Copy misc modules
cp $build_dir/misc_modules/channel_bank/Release/channel_bank.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/discord_integration/Release/discord_integration.dll sdrpp_windows_x64/modules/
cp $build_dir/misc_modules/discord_integration/discord-rpc/src/Release/discord-rpc.dll sdrpp_windows_x64/

cp $build_dir/misc_modules/frequency_manager/Release/frequency_manager.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/iq_exporter/Release/iq_exporter.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/recorder/Release/recorder.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/rigctl_client/Release/rigctl_client.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/rigctl_server/Release/rigctl_server.dll sdrpp_windows_x64/modules/

cp $build_dir/misc_modules/scanner/Release/scanner.dll sdrpp_windows_x64/modules/

# Channel Bank links whisper.cpp and ggml statically on Windows, so there are
# no separate transcription-engine DLLs to package.


# Copy supporting libs
cp 'C:/Program Files/PothosSDR/bin/libusb-1.0.dll' sdrpp_windows_x64/
cp 'C:/Program Files/PothosSDR/bin/pthreadVC2.dll' sdrpp_windows_x64/

Compress-Archive -Path sdrpp_windows_x64/ -DestinationPath sdrpp_windows_x64.zip

rm -Force -Recurse sdrpp_windows_x64
