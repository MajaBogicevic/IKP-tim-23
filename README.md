# Projekat23 – Memory management (Mark–Sweep GC)

Implementiran je rukovalac heap-om zasnovan na mark–sweep algoritmu, sa segmentno organizovanim heap-om.
Skup korena je pojednostavljen korišćenjem kontejnerske strukture, a zatim se radi scan stekova niti radi
iteriranja kroz pokazivače. Heap je thread-safe i meri se propusnost alokacije/dealokacije u 1, 2, 5 i 10 niti.


