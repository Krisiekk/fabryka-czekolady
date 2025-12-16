# Fabryka Czekolady – opis projektu  
**Autor:** Krzysztof Pietrzak  
**Nr albumu:** 156721  
**Nazwa tematu:** FABRYKA_CZEKOLADY  
**Repozytorium GitHub:** https://github.com/Krisiekk/fabryka-czekolady

---

## 1. Opis projektu
Celem projektu jest stworzenie symulacji pracy fabryki czekolady w oparciu o procesy systemu operacyjnego Linux. Każdy „aktor” systemu (dyrektor, magazyn, dostawcy, stanowiska produkcyjne) będzie osobnym procesem, posiadającym własną funkcję `main()` (zgodnie z wymaganiem: unikać rozwiązań scentralizowanych – obowiązkowe użycie `fork()` i `exec()`).

Do komunikacji i synchronizacji zostaną wykorzystane:
- pamięć dzielona (`ftok()`, `shmget()`, `shmat()`, `shmdt()`, `shmctl()`)
- semafory (`ftok()`, `semget()`, `semop()`, `semctl()`)
- sygnały (`signal()`, `sigaction()`, `kill()`, `raise()`)
- możliwe FIFO (`mkfifo()`, `pipe()`) – jeśli wymagane

---

## 2. Opis procesów

| Proces | Zadanie | Funkcje systemowe |
|--------|---------|-------------------|
| `dyrektor` | tworzy procesy i steruje nimi | `fork()`, `exec()`, `wait()` |
| `magazyn` | trzyma składniki w pamięci dzielonej | `shmget()`, `semget()` |
| `dostawca_A/B/C/D` | dostarcza składniki | `fork()`, `exec()`, `shmget()` |
| `stanowisko_1` | produkuje czekoladę typu 1 | `semop()`, `shmget()` |
| `stanowisko_2` | produkuje czekoladę typu 2 | `semop()`, `shmget()` |
| (opcjonalnie) `raport` | zapis do pliku `.txt` | `open()`, `write()` |

---

## 3. Proponowane testy

### ✔ Test 1 – poprawne uruchomienie wszystkich procesów
**Opis:** Dyrektor tworzy magazyn + 1 dostawcę + 1 stanowisko.
**Oczekiwany wynik:** Brak deadlocków, procesy kończą się poprawnie.

### ✔ Test 2 – pełny magazyn (brak miejsca)
**Opis:** Dostawcy próbują dalej dostarczać, mimo braku miejsca.
**Oczekiwany wynik:** procesy czekają na semafor (blokada poprawna).

### ✔ Test 3 – brak składników do produkcji
**Opis:** Stanowisko próbuje produkować – magazyn pusty.
**Oczekiwany wynik:** brak crashów, poprawna obsługa błędu.

### ✔ Test 4 – jednoczesny dostęp wielu procesów
**Opis:** 2 dostawców + 2 stanowiska próbują użyć pamięci jednocześnie.
**Oczekiwany wynik:** semafory blokują konflikt, brak zakleszczeń.

### ✔ Test 5 – kontrola sygnałami
**Opis:** Dyrektor wysyła sygnał STOP/WZNÓW (SIGUSR1/SIGUSR2).
**Oczekiwany wynik:** stanowisko zatrzymuje się i wznawia pracę.


## 4. Struktura repozytorium

fabryka-czekolady/
├── src/               → kod źródłowy (.cpp)
├── include/           → nagłówki (.h)
├── docs/              → opisy projektu / testy
├── build/             → wyniki kompilacji (CMake)
├── CMakeLists.txt
├── README.md
└── .gitignore


## 5. Technologia

**Środowisko systemowe:**
- Host (komputer fizyczny): **macOS**
- Gość (maszyna wirtualna): **Debian GNU/Linux 12 (bookworm)**
- Wirtualizacja: VMware Fusion 
- Środowisko programistyczne: **Visual Studio Code (na Linux VM)**

**Narzędzia:**
- Kompilator: `g++` (C/C++)
- System kompilacji: `cmake` + `make`
- Kontrola wersji: `git`
- Repozytorium: **GitHub – projekt publiczny**





┌─────────────┐
│  DYREKTOR   │ ← Ty wpisujesz komendy 1-4
└──────┬──────┘
       │ fork()+exec()
       ▼
┌──────────────┐    kolejka msg    ┌─────────────┐
│   MAGAZYN    │◄─────────────────►│  DOSTAWCA   │ x4
│              │    semafory       │  (A,B,C,D)  │
│  pamięć shm  │◄─────────────────►└─────────────┘
│              │    
└──────────────┘    kolejka msg    ┌─────────────┐
       ▲        ◄─────────────────►│ STANOWISKO  │ x2
       │           semafory        │   (1, 2)    │
       └───────────────────────────┴─────────────┘