FATE_OGG_FLAC += fate-ogg-flac-chained-meta fate-ogg-flac-chained-copy
fate-ogg-flac-chained-meta: REF = $(SRC_PATH)/tests/ref/fate/ogg-flac-chained-meta.txt
fate-ogg-flac-chained-meta: CMD = run $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) $(TARGET_SAMPLES)/ogg-flac/chained-meta.ogg

fate-ogg-flac-chained-copy: REF = $(SRC_PATH)/tests/ref/fate/ogg-flac-chained-meta.txt
fate-ogg-flac-chained-copy: CMD = ffmpeg -i $(TARGET_SAMPLES)/ogg-flac/chained-meta.ogg -c copy -f ogg - | $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) /dev/stdin

FATE_OGG_FLAC-$(call DEMDEC, OGG, FLAC) += $(FATE_OGG_FLAC)

FATE_SAMPLES_DUMP_STREAM_META += $(FATE_OGG_FLAC-yes)

FATE_EXTERN += $(FATE_OGG_FLAC-yes)

fate-ogg-flac: $(FATE_OGG_FLAC-yes)
