# ESCA Anomalous Sound Detection

The board half. The Raspberry Pi 4 cannot run the model, so this cuts the
microphone stream into segments, asks the AI server what it heard, and puts
the answer on D-Bus for anything that wants to react.

    esca-audiod ──shm ring──► esca-anomalyd ──HTTP──► AI server
                                    │
                            AnomalyEvent on D-Bus

## Running it

    esca-anomalyd --server http://192.168.1.10:8099
    esca-uploader --server http://192.168.1.10:8099 --corpus /var/lib/esca/corpus

The server address lives in `/etc/esca/anomaly.conf`; the systemd unit reads
it from there. The unit is **not** enabled by default: a daemon retrying a
connection to an address nobody has configured is noise, not a feature.

## It asks before it records

`esca-anomalyd` fetches `/v1/anomaly/model` at startup and records at
whatever the server says it wants. The training repository disagrees with
itself about the sample rate — `params.yaml` says 16000, `rt_test.py`
hard-codes 44100 — and the gammatone filterbank is tied to its rate, so the
wrong one scores confidently and wrongly. Publishing the contract and
fetching it beats hard-coding a guess at each end.

If the microphone cannot deliver that rate, the daemon says so and stops
rather than sending audio the model will misread.

## Losing data, on purpose and not

The two paths want opposite behaviour when the link drops, and that is the
main thing this repository gets right:

**Live detection drops.** A verdict about sound from ten minutes ago helps
nobody, so there is no retry queue. Failures are counted, reported through
`FaultEvent`, and the next segment is tried.

**Collected data does not.** The recorder writes to disk first.
`esca-uploader` reads the manifest, sends what has not gone yet, and appends
to `uploaded.txt` **before** any deletion, so a crash costs a duplicate
upload rather than a clip. Nothing is deleted at all unless `--delete-sent`
is passed: disk is cheap next to a corpus that cannot be recorded again.

## The interface

```
service     vn.sparc.esca.Anomaly1
object      /vn/sparc/esca/Anomaly1

method  Start() → i     Stop() → i
        GetState() → (bsutt)      running, model, rate, sent, dropped
        GetErrorMessage() → s

signal  AnomalyEvent(t sequence, t timestamp_ns, b anomaly,
                     d score, d threshold, d inference_ms)
        FaultEvent(s where, s what)
```

`sequence` is the audio chunk the verdict describes, from the same ring the
listener reads, so a viewer can line a verdict up with what it is showing
without synchronising clocks.

## Stop() does not join the worker

It only lowers a flag. sdbus-c++ holds the connection lock for the whole of
a method handler, and the worker emits signals on that same connection, so
joining inside the handler deadlocks both threads and the service never
answers again. The vision service had to be fixed for exactly this; the
join lives in the destructor, where the event loop has already left.
