# Benchmarks

A benchmark based on using TechEmpower's
[plaintext benchmark](https://github.com/TechEmpower/FrameworkBenchmarks/wiki/Project-Information-Framework-Tests-Overview#plaintext)
is used to compare `kestrel` against other HTTP implementations.

This benchmark technically puts `kestrel` at a disadvantage, since `kestrel`
only reads files from disk, while the rest of implementations do not perform any
file I/O for this benchmark (the response is computed in an in-memory buffer).
Even then, as shown below, `kestrel` can compete in terms of throughput and
latency. For fairness, `kestrel` serves a file with the contents expected by
the benchmark. Also, since `kestrel` does not currently generate a `date` HTTP
header in its responses, other implementations were modified to not do it as
well.

The benchmark is biased towards smaller files / transmission sizes. I may
expand this section in the future with more comprehensive data or more server
implementations.

## Parameters

* Test hardware: AMD Ryzen 7 PRO 8840HS (8 physical cores, 16 threads).
* Host kernel: OpenSUSE Leap 6.12
* Client threads: 8
* Server threads: 8
* Connections: 128
* Test duration: 60 seconds

The benchmark is run with [`wrk`](https://github.com/wg/wrk), i.e:

```
nice -20 wrk -t8 -c128 -d60s --latency http://127.0.0.1:8080/plaintext
```

Servers:

* [`faf`](https://github.com/TechEmpower/FrameworkBenchmarks/tree/master/frameworks/Rust/faf):
  Pretty much the theoretical maximum (see [TechEmpower benchmark results](https://www.techempower.com/benchmarks/#section=data-r23&test=plaintext)).
* [`hyper`](https://github.com/TechEmpower/FrameworkBenchmarks/tree/master/frameworks/Rust/hyper/src):
  A popular HTTP implementation in Rust.

### Results

**Test hardware**: AMD Ryzen 7 PRO 8840HS (8 physical cores, 16 threads).

| Server                           | Latency Avg | Latency Stddev | Latency p90 | Latency p99 | Req/Sec   |
| -------------------------------- | ----------- | -------------- | ----------- | ----------- | --------- |
| `faf`                            | 173.33us    | 231.77us       | 236.00us    | 1.35ms      | 931388.29 |
| `kestrel`                        | 208.35us    | 112.31us       | 333.00us    | 453.00us    | 904734.30 |
| `hyper` (current-thread runtime) | 164.11us    | 105.49us       | 278.00us    | 305.00us    | 877907.68 |
| `hyper` (multi-thread runtime)   | 276.76us    | 101.81us       | 389.00us    | 535.00us    | 857604.04 |

The higher latency result is expected, since, as mentioned, `kestrel` is
actually performing file I/O. Even then, `kestrel` performs better in terms
of throughput than `hyper` (~3% improvement) while doing actual file I/O.
