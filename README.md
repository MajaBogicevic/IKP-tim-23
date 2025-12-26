# Projekat23 – Memory management (Mark–Sweep GC)

## Opis
Implementiran je rukovalac heap-om zasnovan na mark–sweep algoritmu, sa segmentno organizovanim heap-om.
Skup korena je pojednostavljen korišćenjem kontejnerske strukture, a zatim se radi scan stekova niti radi
iteriranja kroz pokazivače. Heap je thread-safe i meri se propusnost alokacije/dealokacije u 1, 2, 5 i 10 niti.

## Zahtevi (iz specifikacije)
- Mark–sweep GC za automatsko rukovanje memorijom.
- Roots: kontejnerska struktura umesto kompletnog steka svih niti/registara/globalnih promenljivih.
- Scan stekova niti (iteriranje kroz pokazivače).
- Segmentno organizovan heap.
- Benchmark: throughput alokacije/dealokacije za 1, 2, 5, 10 niti.
- Thread-safe heap.
