FATE_FILTER_DISPLAYMATRIX-$(CONFIG_DISPLAYMATRIX_AUTOROTATE_FILTER) += \
    fate-filter-displaymatrix-90 \
    fate-filter-displaymatrix-90-first \
    fate-filter-displaymatrix-90-middle \
    fate-filter-displaymatrix-180 \
    fate-filter-displaymatrix-270 \
    fate-filter-displaymatrix-hflip \
    fate-filter-displaymatrix-vflip \
    fate-filter-displaymatrix-72-rotw \
    fate-filter-displaymatrix-dynamic \
    fate-filter-displaymatrix-dynamic-hflip-to-vflip \
    fate-filter-displaymatrix-dynamic-vflip-to-hflip \
    fate-filter-displaymatrix-dynamic-180-to-none \
    fate-filter-displaymatrix-dynamic-rotate-72

# Test with 90° rotation matrix applied via filter option (transpose)
fate-filter-displaymatrix-90: CMD = framecrc -lavfi testsrc2=d=0.1:r=5:s=320x240,displaymatrix_autorotate=matrix=0\|65536\|0\|-65536\|0\|0\|0\|0\|1073741824
fate-filter-displaymatrix-90-first: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_90.mp4 -vf displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-90-middle: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_90.mp4 -vf scale=32:24,displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-180: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_180.mp4 -vf displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-270: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_270.mp4 -vf displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-hflip: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/hflip.mp4 -vf displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-vflip: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/vflip.mp4 -vf displaymatrix_autorotate,format=yuv420p
fate-filter-displaymatrix-72-rotw: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_72.mp4 -vf displaymatrix_autorotate=ow=rotw\(a\):oh=roth\(a\),format=yuv420p
# Test dynamic reconfiguration kept (180° hflip+vflip -> 72° rotate, rejected due to dimension change)
fate-filter-displaymatrix-dynamic: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_72.mp4 -vf displaymatrix_autorotate=matrix=-65536\|0\|0\|0\|-65536\|0\|0\|0\|1073741824:ow=rotw\(a\):oh=roth\(a\),format=yuv420p
# Test dynamic reconfiguration accepted (hflip -> vflip, same dimensions)
fate-filter-displaymatrix-dynamic-hflip-to-vflip: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/vflip.mp4 -vf displaymatrix_autorotate=matrix=-65536\|0\|0\|0\|65536\|0\|0\|0\|1073741824,format=yuv420p
# Test dynamic reconfiguration accepted (vflip -> hflip, same dimensions)
fate-filter-displaymatrix-dynamic-vflip-to-hflip: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/hflip.mp4 -vf displaymatrix_autorotate=matrix=65536\|0\|0\|0\|-65536\|0\|0\|0\|1073741824,format=yuv420p
# Test dynamic reconfiguration rejected (180° hflip+vflip -> 90° transpose, dimension change)
fate-filter-displaymatrix-dynamic-180-to-none: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_90.mp4 -vf displaymatrix_autorotate=matrix=-65536\|0\|0\|0\|-65536\|0\|0\|0\|1073741824,format=yuv420p
# Test dynamic reconfiguration accepted (rotate -72° -> rotate 72°, same dimensions with rotw/roth)
fate-filter-displaymatrix-dynamic-rotate-72: CMD = framecrc -noautorotate -i $(TARGET_SAMPLES)/displaymatrix/rotate_72.mp4 -vf displaymatrix_autorotate=matrix=20251\|62328\|0\|-62328\|20251\|0\|0\|0\|1073741824:ow=rotw\(a\):oh=roth\(a\),format=yuv420p

FATE_FILTER-yes += $(FATE_FILTER_DISPLAYMATRIX-yes)
fate-filter-displaymatrix: $(FATE_FILTER_DISPLAYMATRIX-yes)
