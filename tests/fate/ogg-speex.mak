FATE_OGG_SPEEX += fate-ogg-speex-chained-meta fate-ogg-speex-chained-copy
fate-ogg-speex-chained-meta: REF = $(SRC_PATH)/tests/ref/fate/ogg-speex-chained-meta.txt
fate-ogg-speex-chained-meta: CMD = run $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) $(TARGET_SAMPLES)/ogg-speex/chained-meta.ogg

fate-ogg-speex-chained-copy: REF = $(SRC_PATH)/tests/ref/fate/ogg-speex-chained-meta.txt
fate-ogg-speex-chained-copy: CMD = run ffmpeg$(EXESUF) -i $(TARGET_SAMPLES)/ogg-speex/chained-meta.ogg -c copy -f ogg - | $(APITESTSDIR)/api-dump-stream-meta-test$(EXESUF) /dev/stdin

FATE_OGG_SPEEX-$(call DEMDEC, OGG, SPEEX) += $(FATE_OGG_SPEEX)

FATE_SAMPLES_DUMP_STREAM_META += $(FATE_OGG_SPEEX-yes)

FATE_EXTERN += $(FATE_OGG_SPEEX-yes)

fate-ogg-speex: $(FATE_OGG_SPEEX-yes)
