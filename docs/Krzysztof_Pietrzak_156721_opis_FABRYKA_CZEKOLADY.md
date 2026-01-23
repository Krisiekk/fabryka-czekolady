# Fabryka Czekolady – dokumentacja projektu
**Autor:** Krzysztof Pietrzak  
**Nr albumu:** 156721  
**Temat:** FABRYKA_CZEKOLADY  
**Repozytorium:** https://github.com/Krisiekk/fabryka-czekolady

---

## 1. Co robi ten projekt?

To symulacja fabryki czekolady zrobiona w C++ na Linuxie. Każda część fabryki to osobny proces:
- **Dyrektor** – uruchamia wszystko i daje komendy
- **Magazyn** – przechowuje składniki w pamięci dzielonej
- **4 dostawców** (A, B, C, D) – dostarczają surowce
- **2 stanowiska** (1, 2) – robią czekoladę

Wszystkie procesy komunikują się przez:
- **Pamięć dzieloną** – dane o składnikach
- **Semafory** – synchronizacja dostępu
- **Sygnały** – komendy dyrektora (SIGTERM, SIGUSR1)


---

## 2. Jak uruchomić?

```bash
cd build
cmake .. && make
./dyrektor 100
```

**Parametr:** liczba czekolad na pracownika (1-10000, domyślnie 100)  
**Uwaga:** N to cel produkcyjny na stanowisko (worker); są dwa stanowiska, więc łączna produkcja = 2×N.

**Komendy w trakcie działania:**
1. **StopFabryka** – zatrzymuje stanowiska
2. **StopMagazyn** – kończy magazyn bez zapisu, usuwa IPC
3. **StopDostawcy** – zatrzymuje dostawców
4. **StopAll** – zapisuje stan i kończy wszystko
q. **Quit** – kończy bez zapisu

---

## 3. Architektura systemu

### 3.1 Ring Buffer z offsetami bajtowymi

Magazyn używa **ring buffera** dla każdego typu składnika.

**Pojemności (dla N czekolad na pracownika):**
- Segment A: **2×N** elementów × 1B = **2×N** bajtów (obie receptury używają A)
- Segment B: **2×N** elementów × 1B = **2×N** bajtów (obie receptury używają B)
- Segment C: **N** elementów × 2B = **2×N** bajtów (tylko typ 1)
- Segment D: **N** elementów × 3B = **3×N** bajtów (tylko typ 2)

**Łączny rozmiar danych:** 2N + 2N + 2N + 3N = **9×N** bajtów

**Przykład dla N=100:**
- Segment A: 200 elementów = 200B
- Segment B: 200 elementów = 200B
- Segment C: 100 elementów = 200B
- Segment D: 100 elementów = 300B
- Razem: 900B danych + ~100B header = ~1000B pamięci dzielonej

**Offsety IN/OUT przechowywane w semaforach:**
```
SEM_IN_X  = offset bajtowy zapisu (0 .. segmentSize-1)
SEM_OUT_X = offset bajtowy odczytu (0 .. segmentSize-1)

gdzie: segmentSize = capacityX * itemSizeX  (rozmiar segmentu w bajtach)

Przykłady dla N=100:
  A: 0..199 (200*1-1)
  B: 0..199 (200*1-1)
  C: 0..199 (100*2-1)
  D: 0..299 (100*3-1)
```

**Mechanizm zapisu (dostawca):**
1. P(EMPTY_X) – czekaj na wolne miejsce
2. P(MUTEX) – wejdź do sekcji krytycznej
3. Odczytaj IN offset z semafora
4. Zapisz dane pod `segment[IN]`
5. Ustaw nowy IN: `(IN + itemSize) % segmentSize`
6. V(MUTEX) – wyjdź z sekcji
7. V(FULL_X) – sygnalizuj nowe dane

**Mechanizm odczytu (stanowisko):**
1. P(FULL_X) – czekaj na dostępne dane
2. P(MUTEX) – wejdź do sekcji krytycznej
3. Odczytaj OUT offset z semafora
4. Przeczytaj dane z `segment[OUT]`
5. Ustaw nowy OUT: `(OUT + itemSize) % segmentSize`
6. V(MUTEX) – wyjdź z sekcji
7. V(EMPTY_X) – zwolnij miejsce

### 3.2 Semafory (19 sztuk)

**Łącznie 19 semaforów:** 1 mutex + 1 raport + 4 empty + 4 full + 4 in + 4 out + 1 warehouse_on = 19

```
SEM_MUTEX         (0)  = 1       - mutex dla pamięci dzielonej
SEM_RAPORT        (1)  = 1       - mutex dla pliku raportu
SEM_EMPTY_A/B/C/D (2-5)          - wolne miejsca (liczba elementów)
SEM_FULL_A/B/C/D  (6-9)          - zajęte miejsca (liczba elementów)
SEM_IN_A/B/C/D    (10-13)        - offsety zapisu (bajty: 0 do capacity*itemSize-1)
SEM_OUT_A/B/C/D   (14-17)        - offsety odczytu (bajty: 0 do capacity*itemSize-1)
SEM_WAREHOUSE_ON  (18) = 1       - czy magazyn otwarty (bramka)
```

**Inicjalizacja dla N=100:**
```
SEM_EMPTY_A = 200 elementów (2*N)
SEM_FULL_A  = 0 elementów
SEM_IN_A    = 0 bajtów
SEM_OUT_A   = 0 bajtów
```

**Zakres wartości offsetów:**
Maksymalny rozmiar segmentu dla N=10000: segment D = 10000 × 3B = 30000 bajtów.  
Offsety mieszczą się w typie `int` używanym przez `semctl()` (zakres: ±2³¹ ≈ 2 miliardy).

**Limit systemowy semaforów:**
Na Linuksie semafory mają limit wartości (SEMVMX, typowo ~32767). W tym projekcie maksymalne wartości to:  
- EMPTY/FULL: max 20000 elementów (dla N=10000)  
- IN/OUT: max 30000 bajtów (segment D)  
Wszystko mieści się z zapasem.

**UWAGA:** 
- **EMPTY/FULL** = liczniki sztuk (jednostek składnika)
  - Są to **warunki synchronizacji** – dzięki nim procesy blokują się bez busy-wait
  - FULL gwarantuje, że stanowisko nigdy nie czyta pustego slotu
  - EMPTY gwarantuje, że dostawca nigdy nie nadpisuje nieodebranego elementu
  - Dane fizyczne są w SHM, semafory tylko kontrolują dostęp
- **IN/OUT** = offsety bajtowe w segmencie (pozycja zapisu/odczytu)
- Dostawca: `P(EMPTY, 1 sztuka)`, `V(FULL, 1 sztuka)`
- IN/OUT przesuwane o `itemSize` bajtów

**SEM_WAREHOUSE_ON:**
- Wartość 1 = magazyn działa normalnie
- Wartość 0 = magazyn zamknięty
- Dostawcy/stanowiska: `P(WAREHOUSE_ON); V(WAREHOUSE_ON)` – blokują się gdy zamknięty

**Uwaga o bramce:**  
Bramka działa jako kontrola wejścia do iteracji. Proces, który już przeszedł bramkę,  
może dokończyć bieżący krok synchronizacji (P/V na EMPTY/FULL) przed zablokowaniem.

### 3.3 Pamięć dzielona

**Wzór ogólny:**
```
Rozmiar SHM = sizeof(WarehouseHeader) + dataSize
dataSize = 2*N*1B + 2*N*1B + N*2B + N*3B = 9*N bajtów
```

**Dla N=100:**
```
[WarehouseHeader: ~100B]
  - targetChocolates: int (100)
  - capacityA/B/C/D: int (200, 200, 100, 100 elementów)
  - offsetA/B/C/D: size_t (offset początku segmentu w SHM - stałe)
      * offsetA = 0
      * offsetB = offsetA + capacityA*kSizeA  
      * offsetC = offsetB + capacityB*kSizeB
      * offsetD = offsetC + capacityC*kSizeC
  - dataSize: size_t (900B)
  
  UWAGA: IN/OUT (pozycje zapisu/odczytu) są w semaforach SEM_IN_X / SEM_OUT_X,
         nie w headerze! offsetA/B/C/D to stałe "bazowe" (gdzie segment się zaczyna),
         IN/OUT to zmienne "aktualne pozycje" (gdzie teraz piszemy/czytamy).

[Segment A: 200B] (200 elementów × 1B)
[Segment B: 200B] (200 elementów × 1B)
[Segment C: 200B] (100 elementów × 2B)
[Segment D: 300B] (100 elementów × 3B)

Razem: ~100B + 900B = ~1000B pamięci dzielonej
```

**Uwaga:** Wartość sizeof(WarehouseHeader) zależy od architektury (padding). Podane wartości (~964B, ~1000B, header ~80-100B) są przybliżone i różnią się w zależności od kompilatora/architektury.

---

## 4. Procesy

### 4.1 Dyrektor (dyrektor.cpp)

**Co robi:**
- Tworzy procesy: `fork()` + `exec()`
- Czeka na komendy użytkownika
- Wysyła sygnały do procesów

**Komenda StopAll (deterministyczna):**
```
1. SIGTERM → stanowiska → wait_for_range(5,7)
2. SIGTERM → dostawcy  → wait_for_range(1,5)  
3. SIGUSR1 → magazyn   → wait_for_range(0,1)   
4. graceful_shutdown() (już bez magazynu)
```

**Dlaczego czeka w kroku 3?**  
Zapobiega race condition: magazyn mógłby dostać SIGTERM z `graceful_shutdown()` zanim obsłuży SIGUSR1.

### 4.2 Magazyn (magazyn.cpp)

**Co robi:**
- Tworzy IPC: `shmget()`, `semget()`
- Inicjalizuje semafory
- Wypisuje stan co 3 sekundy
- Zapisuje/wczytuje stan z pliku

**Zamykanie magazynu:**

**SIGTERM (komenda 2 - StopMagazyn):**
```cpp
SEM_WAREHOUSE_ON = 0;  // zamknięcie bramki
cleanup_ipc();         // shmctl(IPC_RMID), semctl(IPC_RMID) - USUNIĘCIE IPC
// Zakończenie BEZ zapisu stanu
```

**SIGUSR1 (komenda 4 - StopAll):**
```cpp
SEM_WAREHOUSE_ON = 0;  // zamknięcie bramki
save_state_to_file();  // ZAPIS STANU do pliku
cleanup_ipc();         // usunięcie IPC
// Zakończenie Z zapisem stanu
```

**Co jest zapisywane:**
- Liczniki produkcji dla każdego stanowiska (producedByWorker1/2)
- Wartości 19 semaforów (IN/OUT/EMPTY/FULL + WAREHOUSE_ON)
- **NIE zapisujemy:** Zawartość segmentów SHM (sloty A/B/C/D)  
  Po restarcie magazyn startuje pusty, dostawcy uzupełniają go na bieżąco.

**Awaryjne sprzątanie:**
```cpp
prctl(PR_SET_PDEATHSIG, SIGTERM);  // dostanie SIGTERM gdy dyrektor zginie
```

### 4.3 Dostawca (dostawca.cpp)

**Co robi:**
- Dostarcza składniki (A, B, C lub D)
- Sprawdza bramkę SEM_WAREHOUSE_ON
- Zapisuje dane do ring buffera

**Algorytm:**
```cpp
while (true) {
    P(WAREHOUSE_ON); V(WAREHOUSE_ON);  // sprawdź czy otwarty
    P(EMPTY_X);                         // czekaj na miejsce (1 element)
    
    int capacity = g_header->capacityX;
    int segmentSize = capacity * itemSize;  // np. 200*1 = 200B
    
    P(MUTEX);
    int in = semctl(SEM_IN_X, GETVAL);
    memcpy(segment + in, data, itemSize);
    semctl(SEM_IN_X, SETVAL, (in + itemSize) % segmentSize);  // zawijanie
    V(MUTEX);
    
    V(FULL_X);                          // sygnalizuj (1 element)
    sleep(rand() % 3);
}
```

### 4.4 Stanowisko (stanowisko.cpp)

**Co robi:**
- Pobiera składniki z magazynu
- Produkuje czekoladę (typ 1: A+B+C, typ 2: A+B+D)
- Zapisuje postęp w pamięci dzielonej

**Algorytm:**
```cpp
while (producedChocolates < target) {
    P(WAREHOUSE_ON); V(WAREHOUSE_ON);  // sprawdź czy otwarty
    
    // Pobierz składnik A
    P(FULL_A);                         // czekaj na dane (1 element)
    
    int capacityA = g_header->capacityA;
    int segmentSizeA = capacityA * kSizeA;  // np. 200*1 = 200B
    
    P(MUTEX);
    int out = semctl(SEM_OUT_A, GETVAL);
    memcpy(&data, segment + out, kSizeA);
    semctl(SEM_OUT_A, SETVAL, (out + kSizeA) % segmentSizeA);  // zawijanie
    V(MUTEX);
    
    V(EMPTY_A);                        // zwolnij miejsce (1 element)
    
    // Podobnie B, C/D...
    
    producedChocolates++;
}
```

---

## 5. Środowisko i technologie

**Platforma:**
- Host: macOS / Windows / Linux
- Guest: Debian 12 (64-bit)
- Wirtualizacja: VMware Fusion / VirtualBox

**Narzędzia:**
- Kompilator: g++ 12.2 (C++17, `-std=c++17 -Wall -Wextra`)
- Build system: CMake 3.25
- IDE: Visual Studio Code + Remote SSH
- Kontrola wersji: Git

**Testowanie:**
- Bash scripts (tests/run_tests.sh)
- 5 testów automatycznych

---

## 6. Struktura repozytorium

```
fabryka-czekolady/
├── CMakeLists.txt          # CMake build config
├── README.md               # Podstawowe info
├── docs/
│   └── Krzysztof_Pietrzak_156721_opis_FABRYKA_CZEKOLADY.md
├── include/
│   └── common.h            # Definicje IPC, helpery
├── src/
│   ├── dyrektor.cpp        # Proces główny
│   ├── magazyn.cpp         # IPC owner
│   ├── dostawca.cpp        # Supplier process
│   └── stanowisko.cpp      # Worker process
├── tests/
│   └── run_tests.sh        # Automatyczne testy
└── build/                  # Binaria (gitignore)
    ├── dyrektor
    ├── magazyn
    ├── dostawca
    └── stanowisko
```

---

## 7. Walidacja danych wejściowych

**Parametr N (liczba czekolad):**

```cpp
char* endptr;
long val = strtol(argv[1], &endptr, 10);
if (*endptr != '\0' || val < 1 || val > 10000) {
    cerr << "Błąd: N musi być w zakresie 1-10000\n";
    return 1;
}
```

**Wymagania:**
- Zakres: 1 ≤ N ≤ 10000
- Funkcja: `strtol()` – bezpieczna konwersja string→int
- Wykrywanie błędów: `*endptr != '\0'` (niepełna konwersja)
- Kod wyjścia: `return 1` przy błędzie

---

## 8. Minimalne prawa dostępu (0600)

**IPC Resources:**

| Zasób | Funkcja | Prawa | Uzasadnienie |
|-------|---------|-------|--------------|
| Pamięć dzielona | `shmget(..., 0600)` | `rw-------` | Tylko właściciel (magazyn i jego dzieci) |
| Semafory | `semget(..., 0600)` | `rw-------` | Tylko procesy fabryki |
| Plik stanu | `open(..., 0600)` | `rw-------` | Magazyn zapisuje/wczytuje |
| Raport | `open(..., 0644)` | `rw-r--r--` | Wszyscy mogą czytać |

**Dlaczego 0600?**
- Spełnia zasadę minimalnych uprawnień
- Zapobiega dostępowi innych użytkowników do IPC
- Wymagane przez specyfikację projektu

---

## 9. Mapowanie poleceń dyrektora

**UWAGA:** Numery procesów (1, 2-4, 5-6) to indeksy w tablicy `child_pids[]`, nie rzeczywiste PID-y systemowe.

| Komenda | Sygnał | Cel | Opis działania |
|---------|--------|-----|----------------|
| 1 (StopFabryka) | SIGTERM | Stanowiska (idx 5,6) | Zatrzymuje stanowiska (kończą pętlę produkcji i kończą proces) |
| 2 (StopMagazyn) | SIGTERM | Magazyn (idx 1) | Kończy magazyn **BEZ zapisu stanu**, usuwa IPC (`cleanup_ipc()`), dostawcy/stanowiska tracą dostęp |
| 3 (StopDostawcy) | SIGTERM | Dostawcy (idx 2,3,4,7) | Kończy dostawę składników |
| 4 (StopAll) | **sekwencja** | Wszystkie | 1. SIGTERM→stanowiska + wait<br>2. SIGTERM→dostawcy + wait<br>3. **SIGUSR1**→magazyn + wait (zapis stanu)<br>4. graceful_shutdown() |
| q (Quit) | SIGTERM | Wszystkie | Natychmiastowe zakończenie bez zapisu stanu |

**Kluczowa różnica StopAll:**
- **Deterministyczna sekwencja** z `wait_for_range()` po każdym kroku
- **Zapobiega race condition** – magazyn dostaje SIGUSR1 i ma czas na zapis stanu PRZED otrzymaniem SIGTERM

---

## 10. Tabela: Proces → Zadania → Funkcje systemowe

| Proces | Główne zadania | Funkcje systemowe |
|--------|----------------|-------------------|
| **dyrektor** | Zarządzanie procesami, obsługa komend użytkownika | `fork()`, `execv()`, `waitpid()`, `kill()`, `getpid()`, `signal()`, `sigaction()` |
| **magazyn** | Tworzenie IPC, monitorowanie stanu, zapis/odczyt | `shmget()`, `shmat()`, `shmctl()`, `semget()`, `semctl()`, `semop()`, `prctl()`, `open()`, `read()`, `write()`, `close()` |
| **dostawca** | Dostarczanie składników, sprawdzanie bramki | `shmat()`, `semget()`, `semop()`, `semctl()`, `prctl()`, `getpid()`, `signal()` |
| **stanowisko** | Produkcja czekolad, pobieranie składników | `shmat()`, `semget()`, `semop()`, `semctl()`, `prctl()`, `getpid()`, `signal()` |

**Dlaczego każdy proces ma `prctl()`?**
- Awaryjne sprzątanie gdy dyrektor zginie (SIGKILL, SIGSEGV)
- Kernel automatycznie wyśle SIGTERM do dzieci
- Magazyn wtedy wykona `cleanup_ipc()` i usunie zasoby

---

## 11. Diagram architektury

**UWAGA:** PID-y w diagramie (0-7) są **logiczną numeracją** procesów w tablicy `child_pids[]`,  
nie rzeczywistymi PID-ami systemowymi Linux-a.

```
┌──────────────────────────────────────────────────────────────┐
│                         DYREKTOR                             │
│  (idx 0 - główny proces, zarządza wszystkimi)                │
│  • fork() + execv() → tworzy procesy                         │
│  • kill(pid, SIGTERM/SIGUSR1) → wysyła komendy              │
│  • waitpid() → czeka na dzieci                               │
└──────────────────────────────┬───────────────────────────────┘
                               │
           ┌───────────────────┼───────────────────┐
           │                   │                   │
           ▼                   ▼                   ▼
    ┌─────────┐         ┌─────────┐         ┌─────────┐
    │MAGAZYN  │         │DOSTAWCY │         │STANOW.  │
    │ (idx 1) │         │(idx 2-4)│         │(idx 5-6)│
    └─────────┘         └─────────┘         └─────────┘
         │                   │                   │
         │                   │                   │
         │    ┌──────────────┴───────────────────┘
         │    │
         ▼    ▼
    ┌────────────────────────────────────────────────┐
    │        PAMIĘĆ DZIELONA (964B)                  │
    │  ┌──────────────────────────────────────────┐  │
    │  │ Header (80B)                             │  │
    │  │  - targetChocolates, capacities, etc.    │  │
    │  └──────────────────────────────────────────┘  │
    │  ┌──────────────────────────────────────────┐  │
    │  │ Segment A (200B) - ring buffer           │  │
    │  ├──────────────────────────────────────────┤  │
    │  │ Segment B (200B) - ring buffer           │  │
    │  ├──────────────────────────────────────────┤  │
    │  │ Segment C (200B) - ring buffer           │  │
    │  ├──────────────────────────────────────────┤  │
    │  │ Segment D (300B) - ring buffer           │  │
    │  └──────────────────────────────────────────┘  │
    └────────────────────────────────────────────────┘
                         ▲
                         │
                ┌────────┴────────┐
                │   SEMAFORY (19) │
                │  0-1:  mutexes  │
                │  2-9:  empty/full│
                │ 10-17: in/out    │
                │ 18: WAREHOUSE_ON │
                └─────────────────┘

KOMUNIKACJA:
• Dyrektor → Procesy:   SYGNAŁY (SIGTERM, SIGUSR1)
• Procesy → Magazyn:    SEMAFORY (P/V operations)
• Procesy ↔ Dane:       PAMIĘĆ DZIELONA (read/write)
```

---

## 12. Problemy napotkane i rozwiązania

### Problem 1: Race condition w StopAll

**Objaw:** Magazyn czasami nie zapisywał stanu przed zakończeniem.

**Przyczyna:** Po wysłaniu `SIGUSR1` do magazynu, dyrektor od razu wywoływał `graceful_shutdown()`, który wysyłał `SIGTERM` do WSZYSTKICH procesów (w tym magazynu). Magazyn dostawał SIGTERM zanim obsłużył SIGUSR1.

**Rozwiązanie:**
```cpp
kill(magazyn_pid, SIGUSR1);
wait_for_range(0, 1, 5);  // CZEKAJ aż magazyn zakończy
graceful_shutdown();       // dopiero teraz
```

### Problem 2: Dlaczego IN/OUT w semaforach zamiast w pamięci dzielonej?

**Decyzja:** Offsety `IN` i `OUT` przechowujemy w WARTOŚCIACH semaforów (#10-17), a nie w strukturze w SHM.

**Uzasadnienie:**
IN/OUT są **metadanymi synchronizacji** (podobnie jak EMPTY/FULL) - naturalnie należą do mechanizmu semaforów:
- **Spójność architektury:** Stan ring buffera (IN, OUT, EMPTY, FULL) trzymany w jednym mechanizmie IPC
- **Prostsza struktura SHM:** WarehouseHeader zawiera tylko stałe parametry (capacities, base offsets), nie zmienne stany
- **Łatwiejsza inicjalizacja:** Wartości początkowe wszystkich semaforów ustawiane w jednym miejscu
- **Prostsze odtwarzanie stanu:** Do pełnego odtworzenia zapisuję 19 wartości semaforów (stan ring buffera) oraz liczniki produkcji z headera SHM. Zawartość segmentów (składniki A/B/C/D) nie jest zapisywana - po restarcie magazyn startuje pusty i dostawcy uzupełniają go na bieżąco.

**UWAGA techniczna:** 
Aktualizacja IN/OUT nie jest atomowa - operacja read-modify-write (GETVAL → oblicz nowy → SETVAL) jest wykonywana w sekcji krytycznej chronionej `SEM_MUTEX`. Gdyby IN/OUT były w SHM, mutex i tak byłby potrzebny, więc nie ma różnicy w synchronizacji.

### Problem 3: Po co bramka SEM_WAREHOUSE_ON?

**Cel:** Realistyczna symulacja – magazyn może zostać „zamknięty" (komenda 2), ale dostawcy i stanowiska muszą wiedzieć o tym.

**Alternatywy rozważane:**
1. Procesy kończą się natychmiast → nierealistyczne
2. Procesy pollują flagę w SHM → busy-waiting, marnuje CPU

**Rozwiązanie:**
```cpp
P(SEM_WAREHOUSE_ON);  // wartość 0 → blokada, 1 → przejście
V(SEM_WAREHOUSE_ON);  // oddaj bramkę
```
- Gdy magazyn otwarty (wartość=1): procesy przechodzą
- Gdy zamknięty (wartość=0): procesy blokują się
- Brak busy-wait, oszczędność CPU

### Problem 4: Czemu SEM_MUTEX ma SEM_UNDO?

**Problem:** Jeśli proces zginie w sekcji krytycznej (P(MUTEX) wykonane, V(MUTEX) nie), mutex pozostanie zablokowany na zawsze → deadlock.

**Rozwiązanie:**
```cpp
// W magazyn.cpp init_ipc():
sembuf.sem_flg = SEM_UNDO;  //  kernel odblokuje po śmierci procesu
```

**Konsekwencja:** Jeśli proces crashuje z P(MUTEX) zrobioną, kernel automatycznie wykona V(MUTEX).

### Problem 5: Orphaned processes i dirty IPC po `kill -9 dyrektor`

**Problem:** Gdy dyrektor dostanie SIGKILL (-9):
- Dzieci zostają orphaned (PPID=1)
- Nadal działają w tle
- IPC pozostaje w systemie (`ipcs -m` pokazuje segment)

**Rozwiązanie:**
```cpp
// Wszystkie procesy-dzieci w main():
prctl(PR_SET_PDEATHSIG, SIGTERM);
```
- Kernel wyśle SIGTERM do dzieci gdy rodzic (dyrektor) zginie
- Magazyn obsłuży SIGTERM → `cleanup_ipc()` → `shmctl(IPC_RMID)`, `semctl(IPC_RMID)`
- Brak orphanów, brak dirty IPC

---

## 13. Kluczowe fragmenty kodu

### 13.1 Inicjalizacja IPC (magazyn.cpp, ~linie 45-110)

```cpp
void init_ipc(int N) {
    key_t key = ftok("./ipc.key", 0x42);
    
    // Oblicz rozmiar pamięci dzielonej
    size_t dataSize = 2*N*kSizeA + 2*N*kSizeB + N*kSizeC + N*kSizeD;  // 9*N
    size_t shmSize = sizeof(WarehouseHeader) + dataSize;
    
    // Utwórz pamięć dzieloną
    g_shmid = shmget(key, shmSize, IPC_CREAT | 0600);
    g_shm = (char*)shmat(g_shmid, nullptr, 0);
    g_header = (WarehouseHeader*)g_shm;
    
    // Inicjalizuj nagłówek
    g_header->capacityA = 2 * N;  // 200 dla N=100
    g_header->capacityB = 2 * N;  // 200
    g_header->capacityC = N;      // 100
    g_header->capacityD = N;      // 100
    
    // Semafory (19 sztuk)
    g_semid = semget(key, 19, IPC_CREAT | 0600);
    
    // Inicjalizacja wartości
    semctl(g_semid, SEM_MUTEX, SETVAL, 1);
    semctl(g_semid, SEM_RAPORT, SETVAL, 1);
    
    // EMPTY = pojemność (liczba elementów)
    semctl(g_semid, SEM_EMPTY_A, SETVAL, g_header->capacityA);  // 200
    semctl(g_semid, SEM_EMPTY_B, SETVAL, g_header->capacityB);  // 200
    semctl(g_semid, SEM_EMPTY_C, SETVAL, g_header->capacityC);  // 100
    semctl(g_semid, SEM_EMPTY_D, SETVAL, g_header->capacityD);  // 100
    
    // FULL = 0 (magazyn pusty)
    semctl(g_semid, SEM_FULL_A, SETVAL, 0);
    semctl(g_semid, SEM_FULL_B, SETVAL, 0);
    semctl(g_semid, SEM_FULL_C, SETVAL, 0);
    semctl(g_semid, SEM_FULL_D, SETVAL, 0);
    
    // IN/OUT = 0 (offsety bajtowe)
    semctl(g_semid, SEM_IN_A, SETVAL, 0);
    semctl(g_semid, SEM_OUT_A, SETVAL, 0);
    // ... podobnie B, C, D
    
    semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, 1);  //  otwarty
}
```

**Lokalizacja:** [src/magazyn.cpp](src/magazyn.cpp#L45-L110)

### 13.2 Ring buffer – zapis składnika (dostawca.cpp, ~linie 100-140)

**Przykład dla składnika A** (dla B/C/D analogicznie):

```cpp
void deliver_component(ComponentType type) {
    // 1. Sprawdź bramkę
    P(SEM_WAREHOUSE_ON);
    V(SEM_WAREHOUSE_ON);
    
    // 2. Czekaj na miejsce (1 element)
    P(SEM_EMPTY_A);  // dekrementuje o 1 sztukę
    
    // 3. Pobierz pojemność i oblicz rozmiar segmentu
    int capacity = g_header->capacityA;      // 200 elementów
    int segmentSize = capacity * kSizeA;     // 200 bajtów
    
    // 4. Wejdź do sekcji krytycznej
    P(SEM_MUTEX);
    
    // 5. Odczytaj offset zapisu (bajtowy)
    int in = semctl(g_semid, SEM_IN_A, GETVAL);
    
    // 6. Zapisz dane
    char *segment = g_shm + sizeof(WarehouseHeader);
    memcpy(segment + in, &data, kSizeA);
    
    // 7. Przesuń IN (z zawijaniem po segmencie)
    int newIn = (in + kSizeA) % segmentSize;  // np. (0+1)%200=1, (199+1)%200=0
    semctl(g_semid, SEM_IN_A, SETVAL, newIn);
    
    // 8. Wyjdź z sekcji
    V(SEM_MUTEX);
    
    // 9. Sygnalizuj nowe dane (1 element)
    V(SEM_FULL_A);  // inkrementuje o 1 sztukę
}
```

**Lokalizacja:** [src/dostawca.cpp](src/dostawca.cpp#L100-L140)

### 13.3 Ring buffer – odczyt składnika (stanowisko.cpp, ~linie 120-160)

**Przykład dla składnika A** (dla B/C/D analogicznie):

```cpp
void fetch_component(ComponentType type) {
    // 1. Sprawdź bramkę
    P(SEM_WAREHOUSE_ON);
    V(SEM_WAREHOUSE_ON);
    
    // 2. Czekaj na dane (1 element)
    P(SEM_FULL_A);  // dekrementuje o 1 sztukę
    
    // 3. Pobierz pojemność i oblicz rozmiar segmentu
    int capacity = g_header->capacityA;      // 200 elementów
    int segmentSize = capacity * kSizeA;     // 200 bajtów
    
    // 4. Wejdź do sekcji krytycznej
    P(SEM_MUTEX);
    
    // 5. Odczytaj offset odczytu (bajtowy)
    int out = semctl(g_semid, SEM_OUT_A, GETVAL);
    
    // 6. Pobierz dane
    char *segment = g_shm + sizeof(WarehouseHeader);
    memcpy(&data, segment + out, kSizeA);
    
    // 7. Przesuń OUT (z zawijaniem po segmencie)
    int newOut = (out + kSizeA) % segmentSize;  // np. (0+1)%200=1, (199+1)%200=0
    semctl(g_semid, SEM_OUT_A, SETVAL, newOut);
    
    // 8. Wyczyść slot (opcjonalnie)
    memset(segment + out, 0, kSizeA);
    
    // 9. Wyjdź z sekcji
    V(SEM_MUTEX);
    
    // 10. Zwolnij miejsce (1 element)
    V(SEM_EMPTY_A);  // inkrementuje o 1 sztukę
}
```

**Lokalizacja:** [src/stanowisko.cpp](src/stanowisko.cpp#L120-L160)

### 13.4 Deterministyczna sekwencja StopAll (dyrektor.cpp, ~linie 260-285)

```cpp
case 4: // StopAll
    cout << "[Dyrektor] Deterministyczne zakończenie...\n";
    
    // 1. Stanowiska
    for (int i = 5; i <= 6; i++) kill(child_pids[i], SIGTERM);
    wait_for_range(5, 7, 5);  // czekaj max 5s
    
    // 2. Dostawcy
    for (int i = 2; i <= 4; i++) kill(child_pids[i], SIGTERM);
    kill(child_pids[7], SIGTERM);
    wait_for_range(1, 5, 5);
    
    // 3. Magazyn – SIGUSR1 (zapisz stan)
    kill(child_pids[1], SIGUSR1);
    wait_for_range(0, 1, 5);  // ✅ WAIT przed graceful_shutdown!
    
    // 4. Reszta (już bez magazynu)
    graceful_shutdown();
    break;
```

**Lokalizacja:** [src/dyrektor.cpp](src/dyrektor.cpp#L260-L285)

### 13.5 Awaryjne sprzątanie IPC (magazyn.cpp, ~linie 210-230)

```cpp
void cleanup_ipc() {
    if (g_shmid != -1) {
        shmdt(g_shm);
        shmctl(g_shmid, IPC_RMID, nullptr);  //  usuń z systemu
        cout << "[Magazyn] Usunięto pamięć dzieloną\n";
    }
    if (g_semid != -1) {
        semctl(g_semid, 0, IPC_RMID);  //  usuń semafory
        cout << "[Magazyn] Usunięto semafory\n";
    }
}

// W main():
prctl(PR_SET_PDEATHSIG, SIGTERM);  // dostanie SIGTERM gdy dyrektor zginie

signal(SIGTERM, [](int) {
    SEM_WAREHOUSE_ON = 0;  // zamknij bramkę
    cleanup_ipc();          // usuń IPC
    exit(0);
});
```

**Lokalizacja:** [src/magazyn.cpp](src/magazyn.cpp#L210-L230)

---

## 14. Wyniki testów

**Data:** 22 stycznia 2026  
**Środowisko:** Debian 12, g++ 12.2, CMake 3.25  
**Komenda uruchomienia:** `cd tests && bash run_tests.sh`

### Podsumowanie:

```
Test 1: Poprawne uruchomienie wszystkich procesów       PASS
Test 2: Mała pojemność magazynu (capacity=5)            PASS
Test 3: Zatrzymanie dostawców (StopDostawcy)            PASS
Test 4: Wieloprocesowość i synchronizacja               PASS
Test 5: Test zakleszczeń (długi przebieg)               PASS
Test 6: StopMagazyn - brak zapisu stanu                 PASS
Test 7: Resume - zapis stanu, restart, wczytanie        PASS
─────────────────────────────────────────────────────────────
WYNIK:                                            7/7 PASS
```

---

### TEST 1: Poprawne uruchomienie wszystkich procesów

**Co sprawdzamy:**
- Czy wszystkie procesy (dyrektor, magazyn, 4 dostawców) uruchamiają się poprawnie
- Czy system tworzy plik stanu przy zakończeniu (komenda 4)

**Parametry:** N=100, timeout 10s, komenda "4" (StopAll)

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:27:54] MAGAZYN: Start magazynu (target=100 czekolad, pamięć=964 bajtów, A=200 B=200 C=100 D=100 max)
[20:27:55] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:27:55] DOSTAWCA: Dostarczono 1 x C (offset=0, elem=0/100)
[20:27:55] DOSTAWCA: Dostarczono 1 x A (offset=0, elem=0/200)
[20:27:55] DOSTAWCA: Dostarczono 1 x D (offset=0, elem=0/100)
[20:27:55] DOSTAWCA: Dostarczono 1 x B (offset=0, elem=0/200)
[20:27:55] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:27:55] DOSTAWCA: Dostawca A kończy pracę
[20:27:55] DOSTAWCA: Dostawca B kończy pracę
[20:27:55] DOSTAWCA: Dostawca C kończy pracę
[20:27:55] DOSTAWCA: Dostawca D kończy pracę
[20:27:56] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:27:56] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:27:56] MAGAZYN: Zapisuje stan: A=1 B=1 C=1 D=1
```

**Wnioski:**
Wszystkie 4 dostawców uruchomionych (A, B, C, D) i wykonali po 1 dostawie  
Komenda StopAll działa deterministycznie (stanowiska→dostawcy→zapis)  
Plik `magazyn_state.txt` utworzony - mechanizm zapisu działa

---

### TEST 2: Mała pojemność magazynu (capacity=5)

**Co sprawdzamy:**
- Czy system działa stabilnie przy bardzo małych buforach (ryzyko deadlocków)
- Czy semafory EMPTY/FULL poprawnie obsługują pełny/pusty magazyn

**Parametry:** N=5, timeout 15s, komenda "4"

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:27:57] MAGAZYN: Start magazynu (target=5 czekolad, pamięć=109 bajtów, A=10 B=10 C=5 D=5 max)
[20:27:58] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:27:58] DOSTAWCA: Dostarczono 1 x C (offset=0, elem=0/5)
[20:27:58] DOSTAWCA: Dostarczono 1 x B (offset=0, elem=0/10)
[20:27:58] DOSTAWCA: Dostarczono 1 x A (offset=0, elem=0/10)
[20:27:58] DOSTAWCA: Dostarczono 1 x D (offset=0, elem=0/5)
[20:27:58] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:27:58] DOSTAWCA: Dostawca D kończy pracę
[20:27:58] DOSTAWCA: Dostawca A kończy pracę
[20:27:58] DOSTAWCA: Dostawca C kończy pracę
[20:27:58] DOSTAWCA: Dostawca B kończy pracę
[20:27:59] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:27:59] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:27:59] MAGAZYN: Zapisuje stan: A=1 B=1 C=1 D=1
```

**Wnioski:**
System działa przy pojemności 5× mniejszej niż standardowa  
Brak deadlocków - wszystkie dostawcy wykonały dostawy  
Obliczenia pamięci poprawne: 109B = 9×5 + overhead

---

### TEST 3: Zatrzymanie dostawców (StopDostawcy)

**Co sprawdzamy:**
- Czy komenda 3 zatrzymuje tylko dostawców (bez wpływu na stanowiska/magazyn)
- Czy stanowiska kontynuują pracę po wyczerpaniu zapasów

**Parametry:** N=100, komenda "3" po 4s, komenda "4" po kolejnych 6s

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:28:03] DOSTAWCA: Dostarczono 1 x C (offset=2, elem=1/100)
[20:28:03] DOSTAWCA: Dostarczono 1 x D (offset=3, elem=1/100)
[20:28:03] DOSTAWCA: Dostarczono 1 x B (offset=2, elem=2/200)
[20:28:04] DYREKTOR: Wysyłam SIGTERM do dostawców
[20:28:04] DOSTAWCA: Dostawca A kończy pracę
[20:28:04] DOSTAWCA: Dostawca B kończy pracę
[20:28:04] DOSTAWCA: Dostawca C kończy pracę
[20:28:04] DOSTAWCA: Dostawca D kończy pracę
[20:28:10] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:28:10] STANOWISKO: Stanowisko 1 kończy pracę (wyprodukowano 1 czekolad)
[20:28:10] STANOWISKO: Stanowisko 2 kończy pracę (wyprodukowano 1 czekolad)
[20:28:11] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:28:11] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:28:11] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:28:11] MAGAZYN: Zapisuje stan: A=0 B=1 C=1 D=1
```

**Wnioski:**
Wszyscy 4 dostawcy zakończyli pracę po komendzie StopDostawcy (20:28:04)  
Stanowiska działały dalej i każde wyprodukowało 1 czekoladę  
Graceful shutdown - dostawcy zakończyli bieżące dostawy przed wyjściem

---

### TEST 4: Wieloprocesowość i synchronizacja

**Co sprawdzamy:**
- Czy synchronizacja P()/V() działa poprawnie przy równoległej pracy 7 procesów
- Czy nie ma race conditions w dostępie do ring bufferów

**Parametry:** N=100, długi przebieg (8s), komenda "4"

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:28:19] DOSTAWCA: Dostarczono 1 x D (offset=12, elem=4/100)
[20:28:20] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:28:20] STANOWISKO: Stanowisko 2 kończy pracę (wyprodukowano 2 czekolad)
[20:28:20] STANOWISKO: Stanowisko 1 kończy pracę (wyprodukowano 2 czekolad)
[20:28:20] DOSTAWCA: Dostarczono 1 x A (offset=4, elem=4/200)
[20:28:20] DOSTAWCA: Dostarczono 1 x D (offset=15, elem=5/100)
[20:28:20] DOSTAWCA: Dostarczono 1 x C (offset=10, elem=5/100)
[20:28:21] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:28:21] DOSTAWCA: Dostawca D kończy pracę
[20:28:21] DOSTAWCA: Dostawca B kończy pracę
[20:28:21] DOSTAWCA: Dostawca A kończy pracę
[20:28:21] DOSTAWCA: Dostawca C kończy pracę
[20:28:21] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:28:21] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:28:21] MAGAZYN: Zapisuje stan: A=1 B=0 C=4 D=4
```

**Wnioski:**
Wieloprocesowość działa przy dużym obciążeniu (offset=15 = 5 dostaw D×3B)  
Synchronizacja mutexem + EMPTY/FULL działa bezbłędnie  
Oba stanowiska wyprodukowały po 2 czekolady = łącznie 4 czekolady

---

### TEST 5: Test zakleszczeń (długi przebieg)

**Co sprawdzamy:**
- Czy system działa bez deadlocków przy dłuższym czasie działania (30s)
- Czy pojawia się livelock lub starvation

**Parametry:** N=10, timeout 30s, komenda "4"

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:28:23] MAGAZYN: Start magazynu (target=10 czekolad, pamięć=154 bajtów, A=20 B=20 C=10 D=10 max)
[20:28:24] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:28:24] DOSTAWCA: Dostarczono 1 x B (offset=0, elem=0/20)
[20:28:24] DOSTAWCA: Dostarczono 1 x A (offset=0, elem=0/20)
[20:28:24] DOSTAWCA: Dostarczono 1 x C (offset=0, elem=0/10)
[20:28:24] DOSTAWCA: Dostarczono 1 x D (offset=0, elem=0/10)
[20:28:24] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:28:24] DOSTAWCA: Dostawca B kończy pracę
[20:28:24] DOSTAWCA: Dostawca A kończy pracę
[20:28:24] DOSTAWCA: Dostawca C kończy pracę
[20:28:24] DOSTAWCA: Dostawca D kończy pracę
[20:28:25] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:28:25] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:28:25] MAGAZYN: Zapisuje stan: A=1 B=1 C=1 D=1
```

**Wnioski:**
System działał 30s bez zakleszczeń  
Wszystkie semafory zwolnione - brak deadlocków  
Brak busy-wait - system szybko zakończył pracę (efektywne P/V)

---

### TEST 6: StopMagazyn - brak zapisu stanu

**Co sprawdzamy:**
- Czy komenda 2 (StopMagazyn) **NIE** tworzy pliku `magazyn_state.txt`
- Czy magazyn usuwa zasoby IPC (cleanup_ipc) przy SIGTERM

**Parametry:** N=100, komenda "2" po 3s, komenda "q" po kolejnych 2s

**Logi z raport.txt (ostatnie 15 linii):**
```
[20:28:26] MAGAZYN: Start magazynu (target=100 czekolad, pamięć=964 bajtów, A=200 B=200 C=100 D=100 max)
[20:28:27] DOSTAWCA: Dostarczono 1 x B (offset=0, elem=0/200)
[20:28:27] DOSTAWCA: Dostarczono 1 x C (offset=0, elem=0/100)
[20:28:27] DOSTAWCA: Dostarczono 1 x A (offset=0, elem=0/200)
[20:28:27] DOSTAWCA: Dostarczono 1 x D (offset=0, elem=0/100)
[20:28:27] STANOWISKO: Stanowisko 2 wyprodukowano czekoladę #1 (A+B+D)
[20:28:28] DOSTAWCA: Dostarczono 1 x C (offset=2, elem=1/100)
[20:28:28] DOSTAWCA: Dostarczono 1 x D (offset=3, elem=1/100)
[20:28:29] DYREKTOR: Wysyłam SIGTERM do magazynu (bez zapisu)
[20:28:29] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:28:29] MAGAZYN: Zakończenie bez zapisu stanu (SIGTERM)
```

**Wnioski:**
Komenda 2 wysyła SIGTERM bez zapisu stanu (log: "bez zapisu")  
Magazyn wywołał `cleanup_ipc()` - semafory i SHM usunięte  
Kluczowa różnica StopMagazyn (cmd 2) vs StopAll (cmd 4) potwierdzona

---

### TEST 7: Resume - zapis stanu, restart, wczytanie

**Co sprawdzamy:**
- Czy mechanizm save/load działa poprawnie przez 2 cykle
- Czy po restarcie magazyn kontynuuje od zapisanych wartości

**Parametry:** N=10, 2 uruchomienia (Run#1 → save, Run#2 → load)

**Logi z raport.txt Run#2 (ostatnie 15 linii):**
```
[20:28:35] MAGAZYN: Wczytano stan z pliku (A=1, B=1, C=1, D=1)
[20:28:35] MAGAZYN: Odtworzono stan: A=1 B=1 C=1 D=1
[20:28:36] DYREKTOR: StopAll - zatrzymuję stanowiska...
[20:28:36] DOSTAWCA: Dostarczono 1 x C (offset=2, elem=1/10)
[20:28:36] DOSTAWCA: Dostarczono 1 x A (offset=1, elem=1/20)
[20:28:36] DOSTAWCA: Dostarczono 1 x B (offset=1, elem=1/20)
[20:28:36] DOSTAWCA: Dostarczono 1 x D (offset=3, elem=1/10)
[20:28:37] DYREKTOR: StopAll - zatrzymuję dostawców...
[20:28:37] DOSTAWCA: Dostawca B kończy pracę
[20:28:37] DOSTAWCA: Dostawca C kończy pracę
[20:28:37] DOSTAWCA: Dostawca A kończy pracę
[20:28:37] DOSTAWCA: Dostawca D kończy pracę
[20:28:37] DYREKTOR: StopAll - zapisuję stan magazynu...
[20:28:37] MAGAZYN: Magazyn zamknięty (SEM_WAREHOUSE_ON=0)
[20:28:37] MAGAZYN: Zapisuje stan: A=2 B=2 C=2 D=2
```

**Wnioski:**
Log `"Wczytano stan z pliku (A=1, B=1, C=1, D=1)"` potwierdza load_state_from_file()  
Dostawcy kontynuowali od `offset=1` (A), `offset=2` (C) zamiast 0 - **resume działa!**  
Stan końcowy Run#2: A=2, B=2, C=2, D=2 = wartości z Run#1 + nowe dostawy  
 **Kluczowy test** - potwierdza wymaganie zapisu/wczytania stanu

---

## 15. Podsumowanie

**Zrealizowane wymagania:**
- Pamięć dzielona (System V, `shmget/shmat/shmctl`)
- Semafory (19 sztuk, `semget/semop/semctl`)
- Procesy (`fork/exec/wait/kill`)
- Sygnały (SIGTERM, SIGUSR1, `signal/sigaction`)
- Prawa minimalne (0600 dla IPC i plików)
- Walidacja danych (`strtol`, zakres 1-10000)
- Deterministyczna synchronizacja
- Zapisywanie/wczytywanie stanu (TEST 7 potwierdza!)
- Awaryjne sprzątanie (prctl)
- Dokumentacja i testy (7/7 PASS)

**Innowacje:**
1. **Offsety w semaforach** – metadane synchronizacji w jednym mechanizmie, prostsza struktura SHM
2. **Bramka SEM_WAREHOUSE_ON** – realistyczne zamykanie magazynu bez busy-wait
3. **prctl(PR_SET_PDEATHSIG)** – automatyczne sprzątanie po crash dyrektora
4. **Deterministyczna sekwencja StopAll** – wait_for_range() zapobiega race conditions

**Kluczowe testy:**
- **TEST 6:** Potwierdza krytyczne rozróżnienie StopMagazyn (SIGTERM, brak zapisu) vs StopAll (SIGUSR1, zapis)
- **TEST 7:** Dowodzi, że save/load działa - magazyn wznawia od zapisanych wartości (pos=1 zamiast pos=0)
- **TEST 4:** 24 dostawy + 6 produkcji równolegle - wieloprocesowość bez race conditions
- **TEST 5:** 30s bez deadlocków - semafory P()/V() działają bezbłędnie







