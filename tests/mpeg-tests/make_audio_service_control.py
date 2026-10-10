"""Restore only the audio UPDATE service change in a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

def remove_once(text):
    global source
    if source.count(text) != 1:
        raise SystemExit("Expected exactly one audio-service control marker: " + text[:80])
    source = source.replace(text, "", 1)

def replace_once(old, new):
    global source
    if source.count(old) != 1:
        raise SystemExit("Expected exactly one audio-service control block: " + old[:80])
    source = source.replace(old, new, 1)

remove_once("            uint64_t lastOutputServiceTick = std::numeric_limits<uint64_t>::max();\n")
start = source.index("        void serviceRejectedAudioOutput(")
end = source.index("        void acceptDemuxCallback(", start)
remove_once(source[start:end])
remove_once("            MpegStreamCallbackEvent outputService;\n")
start = source.index("                    const uint32_t type = transaction->events[transaction->eventIndex].streamType;")
end = source.index("\n                }\n                else\n                    ++transaction->callbackIndex;", start)
remove_once(source[start:end])
replace_once("""            if (rejected)
            {
                if (outputService.callbacks.empty())
                    finishDemuxTransaction(transaction);
                else
                    serviceRejectedAudioOutput(transaction, outputService, 0u);
            }
""", """            if (rejected)
                finishDemuxTransaction(transaction);
""")
remove_once("                playback.lastOutputServiceTick = currentTick;\n")
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
with destination.open("w", encoding="utf-8", newline="\n") as output:
    output.write(source)
