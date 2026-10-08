"""L0 wrapper: events_codec with the contexts restarted at the first stream block and at each
block a snapshot follows (the README's headline figure, events_codec_snapreset.txt)."""
import events_codec, combined
events_codec.RESETS = combined.snapshot_resets()
print("reset blocks:", sorted(events_codec.RESETS))
events_codec.main()
