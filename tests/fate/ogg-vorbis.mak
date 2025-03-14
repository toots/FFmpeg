FATE_OGG_VORBIS += fate-ogg-vorbis-chained-meta fate-ogg-vorbis-chained-copy
fate-ogg-vorbis-chained-meta: REF = $(SRC_PATH)/tests/ref/fate/ogg-vorbis-chained-meta.txt
fate-ogg-vorbis-chained-meta: CMD = run $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) $(TARGET_SAMPLES)/ogg-vorbis/chained-meta.ogg

fate-ogg-vorbis-chained-copy: REF = $(SRC_PATH)/tests/ref/fate/ogg-vorbis-chained-meta.txt
fate-ogg-vorbis-chained-copy: CMD = ffmpeg -i $(TARGET_SAMPLES)/ogg-vorbis/chained-meta.ogg -c copy -f ogg - | $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) /dev/stdin

FATE_OGG_VORBIS-$(call DEMDEC, OGG, VORBIS) += $(FATE_OGG_VORBIS)

FATE_SAMPLES_DUMP_STREAM_META += $(FATE_OGG_VORBIS-yes)

FATE_EXTERN += $(FATE_OGG_VORBIS-yes)

fate-ogg-vorbis: $(FATE_OGG_VORBIS-yes)
