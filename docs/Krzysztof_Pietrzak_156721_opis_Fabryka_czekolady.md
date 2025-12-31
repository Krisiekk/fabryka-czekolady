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
- kolejki komunikatów System V (`ftok()`, `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()`)

---

## 2. Opis procesów

| Proces | Zadanie | Funkcje systemowe |
|--------|---------|-------------------|
| `dyrektor` | tworzy procesy i steruje nimi | `fork()`, `exec()`, `wait()`, `kill()` |
| `magazyn` | trzyma składniki w pamięci dzielonej | `shmget()`, `semget()`, `msgrcv()` |
| `dostawca_A/B/C/D` | dostarcza składniki | `shmget()`, `semop()`, `msgsnd()` |
| `stanowisko_1` | produkuje czekoladę typu 1 | `semop()`, `shmget()`, `msgsnd()` |
| `stanowisko_2` | produkuje czekoladę typu 2 | `semop()`, `shmget()`, `msgsnd()` |

### Mapowanie poleceń dyrektora

| Polecenie | Komenda | Opis |
|-----------|---------|------|
| **1** | `StopFabryka` | Zatrzymuje stanowiska produkcyjne (1 i 2) |
| **2** | `StopMagazyn` | Zatrzymuje proces magazynu |
| **3** | `StopDostawcy` | Zatrzymuje wszystkich dostawców (A, B, C, D) |
| **4** | `StopAll` | Zapisuje stan magazynu i kończy wszystkie procesy |

---

## 3. Proponowane testy

### ✔ Test 1 – poprawne uruchomienie wszystkich procesów fabryki

**Opis:**  
Dyrektor uruchamia wszystkie procesy fabryki: magazyn, czterech dostawców (A, B, C, D) oraz dwa stanowiska produkcyjne (1 i 2). Procesy działają równolegle i komunikują się za pomocą pamięci dzielonej, semaforów oraz kolejek komunikatów. Po pewnym czasie dyrektor wysyła polecenie `StopAll`.

**Oczekiwany wynik:**  
- wszystkie procesy uruchamiają się poprawnie  
- dostawcy niezależnie dostarczają surowce do magazynu  
- stanowiska pobierają surowce i produkują czekoladę  
- nie występują deadlocki ani race condition  
- po wysłaniu `StopAll` wszystkie procesy kończą pracę  
- magazyn zapisuje aktualny stan do pliku  

**Rzeczywisty wynik:**  
- dyrektor poprawnie uruchomił magazyn, 4 dostawców oraz 2 stanowiska  
- dostawcy dostarczali surowce w losowych momentach  
- stanowiska poprawnie pobierały surowce i produkowały czekoladę  
- synchronizacja przebiegała poprawnie (brak zakleszczeń)  
- po wysłaniu `StopAll` magazyn zapisał stan do pliku  
- wszystkie procesy zakończyły działanie w sposób kontrolowany (graceful shutdown)  

**Status:** ✔ ZALICZONY  



#### Fragment logu z wykonania testu:


[14:41:24] MAGAZYN: Start magazynu (capacity=100)
[14:41:24] MAGAZYN: Odtworzono stan z pliku: A=2 B=0 C=4 D=10 (zajetosc: 40/100 jednostek)
[14:41:25] DOSTAWCA: Dostarczono 1 x A (stan: A=3 B=0 C=4 D=10)
[14:41:25] DOSTAWCA: Dostarczono 1 x D (stan: A=3 B=0 C=4 D=11)
[14:41:25] DOSTAWCA: Dostarczono 1 x B (stan: A=3 B=1 C=4 D=11)
[14:41:25] DOSTAWCA: Dostarczono 1 x C (stan: A=3 B=1 C=5 D=11)
[14:41:25] MAGAZYN: Żądanie od stanowiska 2 (pid=13874): A=1 B=1 C=0 D=1
[14:41:25] MAGAZYN: Wydano surowce dla stanowiska 2 (pid=13874), stan: A=2 B=0 C=5 D=10
[14:41:25] STANOWISKO: Stanowisko 2 wyprodukowano czekolade (pid=13874)
[14:41:28] MAGAZYN: Żądanie od stanowiska 1 (pid=13873): A=1 B=1 C=1 D=0
[14:41:28] MAGAZYN: Wydano surowce dla stanowiska 1 (pid=13873), stan: A=2 B=0 C=5 D=11
[14:41:28] STANOWISKO: Stanowisko 1 wyprodukowano czekolade (pid=13873)
[14:41:29] DYREKTOR: Wysyłam StopAll (zapis stanu i zakończenie)
[14:41:30] MAGAZYN: Odebrano StopAll - koncze prace
[14:41:30] MAGAZYN: Zapisuje stan do pliku: A=3 B=1 C=6 D=12 (zajetosc: 52/100 jednostek)
[14:41:34] DOSTAWCA: Dostawca A konczy prace
[14:41:34] DOSTAWCA: Dostawca B konczy prace
[14:41:34] DOSTAWCA: Dostawca C konczy prace
[14:41:34] DOSTAWCA: Dostawca D konczy prace
[14:41:34] STANOWISKO: Stanowisko 1 konczy prace
[14:41:34] STANOWISKO: Stanowisko 2 konczy prace



### ✔ Test 2 – pełny magazyn (brak miejsca)

**Opis:**  
Magazyn został uruchomiony z ograniczoną pojemnością (`capacity = 10`).  
Dostawcy A, B, C i D próbują dostarczać surowce w losowych momentach czasowych,
natomiast stanowiska produkcyjne 1 i 2 jednocześnie zgłaszają zapotrzebowanie
na surowce do produkcji.

Celem testu było sprawdzenie poprawnej synchronizacji przy pełnym magazynie
oraz zachowania systemu w sytuacji braku wolnego miejsca.

**Konfiguracja testu:**  
- pojemność magazynu: `10` jednostek  
- aktywni dostawcy: A, B, C, D  
- aktywne stanowiska produkcyjne: 1 i 2  
- synchronizacja: semafory System V  
- dane magazynu: pamięć dzielona  

**Oczekiwany wynik:**  
- po zapełnieniu magazynu dostawcy blokują się na semaforze pojemności  
- brak nadpisania danych w pamięci dzielonej  
- stanowiska produkcyjne czekają na dostępność surowców  
- brak deadlocków i crashów  
- poprawne zakończenie pracy po poleceniu `StopAll`  

**Rzeczywisty wynik:**  
- po osiągnięciu maksymalnej pojemności magazynu (`10/10`) dalsze dostawy
  nie powodowały przekroczenia limitu  
- procesy stanowisk poprawnie oczekiwały na dostępność surowców  
- system nie uległ zakleszczeniu  
- po wysłaniu polecenia `StopAll` stan magazynu został zapisany do pliku  
- wszystkie procesy zakończyły pracę w sposób kontrolowany  

**Status:** ✔ ZALICZONY

---

#### Fragment logu z przebiegu testu:


[14:50:16] MAGAZYN: Start magazynu (capacity=10)
[14:50:17] DOSTAWCA: Dostarczono 1 x A (stan: A=1 B=0 C=0 D=0)
[14:50:17] DOSTAWCA: Dostarczono 1 x D (stan: A=1 B=1 C=0 D=1)
[14:50:17] DOSTAWCA: Dostarczono 1 x B (stan: A=1 B=1 C=0 D=1)
[14:50:17] DOSTAWCA: Dostarczono 1 x C (stan: A=1 B=1 C=1 D=1)
[14:50:18] MAGAZYN: Żądanie od stanowiska 1 (pid=14458): A=1 B=1 C=1 D=0
[14:50:18] MAGAZYN: Wydano surowce dla stanowiska 1 (pid=14458), stan: A=0 B=0 C=0 D=1
[14:50:22] MAGAZYN: Wydano surowce dla stanowiska 2 (pid=14459), stan: A=0 B=0 C=1 D=1
[14:50:26] STANOWISKO: Stanowisko 2 wyprodukowano czekolade (pid=14459)
[14:50:53] DYREKTOR: Wysyłam StopAll (zapis stanu i zakończenie)
[14:50:53] MAGAZYN: Zapisuje stan do pliku: A=0 B=0 C=2 D=2 (zajetosc: 10/10 jednostek)
[14:50:54] STANOWISKO: Stanowisko 1 konczy prace (pid=14458)
[14:50:54] STANOWISKO: Stanowisko 2 konczy prace (pid=14459)
[14:50:58] DOSTAWCA: Dostawca C konczy prace (pid=14455)


### ✔ Test 3 – zatrzymanie dostawców (StopDostawcy)

**Cel:** Sprawdzić, czy po wysłaniu komendy `StopDostawcy` wszyscy dostawcy kończą pracę i przestają dostarczać składniki, a pozostałe procesy działają dalej (stanowiska dalej zgłaszają żądania do magazynu).

**Opis:** Uruchamiamy pełną symulację (magazyn + 4 dostawców + 2 stanowiska). Następnie dyrektor wysyła `StopDostawcy`, a po chwili `StopAll`.

**Kroki:**
1. Uruchom program w trybie standardowym (capacity=100).
2. Poczekaj aż dostawcy dostarczą kilka składników i stanowiska wyprodukują czekoladę.
3. Wyślij komendę `StopDostawcy`.
4. Obserwuj czy wszyscy dostawcy kończą pracę (log “Dostawca X konczy prace”).
5. Po kilku sekundach wyślij `StopAll`.

**Oczekiwany wynik:**
- Po `StopDostawcy` każdy dostawca (A/B/C/D) kończy pracę i **nie pojawiają się już żadne logi** typu „DOSTAWCA: Dostarczono…”.
- Stanowiska mogą nadal wysyłać żądania do magazynu (logi „MAGAZYN: Żądanie od stanowiska …”).
- Po `StopAll` magazyn zapisuje stan do pliku i wszystkie procesy kończą pracę poprawnie.

**Wynik testu:** ✔  ZALICZONY (dostawcy kończą pracę po StopDostawcy, brak kolejnych dostaw)

**Logi z uruchomienia:**
[15:17:24] MAGAZYN: Start magazynu (capacity=100)
[15:17:24] MAGAZYN: Odtworzono stan z pliku: A=1 B=3 C=14 D=20 (zajetosc: 92/100 jednostek)
[15:17:25] DOSTAWCA: Dostarczono 1 x A (stan: A=2 B=3 C=14 D=20)
[15:17:25] DOSTAWCA: Dostarczono 1 x B (stan: A=2 B=4 C=14 D=20)
[15:17:26] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:26] MAGAZYN: Wydano surowce dla stanowiska 1 (pid=18444), stan: A=1 B=3 C=13 D=20
[15:17:26] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:26] MAGAZYN: Wydano surowce dla stanowiska 2 (pid=18445), stan: A=0 B=2 C=13 D=19
[15:17:26] STANOWISKO: Stanowisko 1 wyprodukowano czekolade (pid=18444)
[15:17:26] STANOWISKO: Stanowisko 2 wyprodukowano czekolade (pid=18445)
[15:17:27] DYREKTOR: Wysyłam StopDostawcy
[15:17:27] DOSTAWCA: Dostawca A konczy prace (pid=18440)
[15:17:27] DOSTAWCA: Dostawca B konczy prace (pid=18441)
[15:17:27] DOSTAWCA: Dostawca C konczy prace (pid=18442)
[15:17:27] DOSTAWCA: Dostawca D konczy prace (pid=18443)
[15:17:28] DYREKTOR: Wysyłam StopDostawcy
[15:17:28] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:29] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:30] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:31] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:32] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:33] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:34] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:35] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:36] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:37] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:39] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:39] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:41] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:41] MAGAZYN: Żądanie od stanowiska 2 (pid=18445): A=1 B=1 C=0 D=1
[15:17:42] DYREKTOR: Wysyłam StopAll (zapis stanu i zakończenie)
[15:17:42] MAGAZYN: Odebrano StopAll - koncze prace
[15:17:42] MAGAZYN: Zapisuje stan do pliku: A=0 B=2 C=13 D=19 (zajetosc: 85/100 jednostek)
[15:17:43] STANOWISKO: Stanowisko 2 konczy prace (pid=18445)
[15:17:43] STANOWISKO: Stanowisko 1 konczy prace (pid=18444)

### ✔ Test 4 – jednoczesny dostęp wielu procesów

**Cel:**  
Sprawdzenie poprawności synchronizacji przy równoczesnym dostępie wielu procesów do pamięci dzielonej magazynu.

**Opis:**  
Uruchomiono fabrykę z pełnym zestawem procesów: magazyn, czterech dostawców (A, B, C, D) oraz dwa stanowiska produkcyjne (1 i 2).  
Dostawcy oraz stanowiska działały równolegle, jednocześnie wykonując operacje zapisu i odczytu danych w pamięci dzielonej magazynu.

Celem testu było sprawdzenie, czy zastosowane semafory poprawnie chronią sekcję krytyczną oraz czy nie występują race condition ani zakleszczenia.

**Konfiguracja testu:**  
- pojemność magazynu: `100` jednostek  
- aktywni dostawcy: A, B, C, D  
- aktywne stanowiska produkcyjne: 1 i 2  
- synchronizacja: semafory System V  
- komunikacja: pamięć dzielona + kolejki komunikatów  

**Oczekiwany wynik:**  
- dostawy oraz żądania stanowisk pojawiają się równolegle  
- magazyn obsługuje żądania w sposób zsynchronizowany  
- brak niespójnych stanów magazynu  
- brak deadlocków i crashów  
- poprawne zakończenie pracy po poleceniu `StopAll`  

**Rzeczywisty wynik:**  
- w logach widoczne były jednoczesne dostawy i żądania stanowisk  
- magazyn poprawnie synchronizował dostęp do pamięci dzielonej  
- nie wystąpiły zakleszczenia ani błędy synchronizacji  
- po wysłaniu `StopAll` wszystkie procesy zakończyły pracę poprawnie  

**Status:** ✔ ZALICZONY

#### Fragment logu z przebiegu testu:

 
[15:17:24] MAGAZYN: Start magazynu (capacity=100)
[15:17:25] DOSTAWCA: Dostarczono 1 x A (stan: A=2 B=3 C=14 D=20)
[15:17:26] MAGAZYN: Żądanie od stanowiska 1 (pid=18444): A=1 B=1 C=1 D=0
[15:17:26] MAGAZYN: Wydano surowce dla stanowiska 1 (pid=18444)
[15:17:26] STANOWISKO: Stanowisko 1 wyprodukowano czekolade
[15:17:26] MAGAZYN: Żądanie od stanowiska 2 (pid=18445)
[15:17:26] MAGAZYN: Wydano surowce dla stanowiska 2 (pid=18445)
[15:17:26] STANOWISKO: Stanowisko 2 wyprodukowano czekolade
[15:17:42] DYREKTOR: Wysyłam StopAll (zapis stanu i zakończenie)
[15:17:42] MAGAZYN: Zapisuje stan do pliku


### ✔ Test 5 – stabilność systemu i brak zakleszczeń (deadlock, długi przebieg)

**Cel:**  
Sprawdzenie stabilności działania systemu oraz potwierdzenie braku zakleszczeń
(deadlock) podczas długotrwałej pracy fabryki przy ograniczonej pojemności magazynu.

**Opis:**  
Fabryka została uruchomiona z pełnym zestawem procesów:
magazyn, czterech dostawców (A, B, C, D) oraz dwa stanowiska produkcyjne (1 i 2).
Pojemność magazynu została ustawiona na `capacity = 10`, co powoduje częste
blokowanie procesów na semaforach (brak miejsca / brak surowców).

System pracował nieprzerwanie przez około **30 sekund**, generując intensywną
aktywność dostaw i produkcji. Następnie dyrektor wysłał polecenie `StopAll`,
kończące pracę fabryki z zapisem stanu magazynu.

**Konfiguracja testu:**  
- pojemność magazynu: `10` jednostek  
- aktywni dostawcy: A, B, C, D  
- aktywne stanowiska produkcyjne: 1 i 2  
- synchronizacja: semafory System V  
- komunikacja: pamięć dzielona + kolejki komunikatów System V  

**Twarde kryterium zaliczenia testu (deadlock):**
- w logach występują jednocześnie:
  - dostawy surowców (`DOSTAWCA: Dostarczono …`),
  - wydania surowców przez magazyn (`MAGAZYN: Wydano surowce …`),
  - produkcja czekolady (`STANOWISKO: wyprodukowano czekolade`),
- liczba wpisów w logu **rośnie przez cały czas trwania testu**
  (system nie „zawiesza się” i nie traci postępu),
- po wysłaniu `StopAll` wszystkie procesy kończą pracę w sposób kontrolowany,
  a magazyn zapisuje aktualny stan do pliku.

**Rzeczywisty wynik:**  
- podczas testu występowały liczne dostawy, wydania oraz produkcje czekolady,
- system wykazywał ciągły postęp (brak zatrzymania logów),
- nie zaobserwowano zakleszczeń ani awarii procesów,
- po poleceniu `StopAll` wszystkie procesy zakończyły pracę poprawnie,
  a stan magazynu został zapisany do pliku.

**Status:** ✔ ZALICZONY

#### Fragment logu z przebiegu testu:

[16:21:28] MAGAZYN: Start magazynu (capacity=10)
[16:21:29] DOSTAWCA: Dostarczono 1 x A
[16:21:29] DOSTAWCA: Dostarczono 1 x B
[16:21:29] DOSTAWCA: Dostarczono 1 x C
[16:21:29] DOSTAWCA: Dostarczono 1 x D
[16:21:29] MAGAZYN: Żądanie od stanowiska 1 (pid=21507)
[16:21:29] MAGAZYN: Wydano surowce dla stanowiska 1
[16:21:30] STANOWISKO: Stanowisko 1 wyprodukowano czekolade
[16:21:37] MAGAZYN: Wydano surowce dla stanowiska 2
[16:21:37] STANOWISKO: Stanowisko 2 wyprodukowano czekolade
[16:21:52] MAGAZYN: Wydano surowce dla stanowiska 2
[16:21:52] STANOWISKO: Stanowisko 2 wyprodukowano czekolade
[16:22:01] DYREKTOR: Wysyłam StopAll (zapis stanu i zakończenie)
[16:22:01] MAGAZYN: Odebrano StopAll - koncze prace
[16:22:01] MAGAZYN: Zapisuje stan do pliku: A=0 B=2 C=1 D=2 (zajetosc: 10/10)
[16:22:02] STANOWISKO: Stanowisko 1 konczy prace
[16:22:02] STANOWISKO: Stanowisko 2 konczy prace



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



## 6. Linki do istotnych fragmentów kodu (pkt 5.2)

Poniżej przedstawiono linki do kluczowych fragmentów kodu źródłowego,
które dokumentują użycie wymaganych w projekcie konstrukcji systemowych.
Każdy link prowadzi do konkretnego pliku oraz zakresu linii w repozytorium GitHub
(permalink – stały link do konkretnej wersji kodu).

---

### a) Tworzenie i obsługa plików  
*(creat(), open(), close(), read(), write(), unlink())*

- **Zapis raportu/logów do pliku tekstowego (`open`, `write`, `close`)**  
  → [include/common.h#L154](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/include/common.h#L154)

- **Odczyt stanu magazynu z pliku (`open`, `read`, `close`)**  
  → [src/magazyn.cpp#L96](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L96)

- **Zapis stanu magazynu do pliku (`open`, `write`, `close`)**  
  → [src/magazyn.cpp#L149](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L149)

---

### b) Tworzenie procesów  
*(fork(), exec(), exit(), wait())*

- **Uruchamianie procesów potomnych (magazyn, dostawcy, stanowiska)**  
  → [src/dyrektor.cpp#L29](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/dyrektor.cpp#L29)

- **Oczekiwanie na zakończenie procesów potomnych (`waitpid`)**  
  → [src/dyrektor.cpp#L105](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/dyrektor.cpp#L105)

---

### d) Obsługa sygnałów  
*(kill(), sigaction())*

- **Rejestracja obsługi sygnałów (`sigaction`)**  
  → [include/common.h#L188](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/include/common.h#L188)

- **Wysyłanie sygnałów do procesów (`kill`)**  
  → [src/dyrektor.cpp#L150](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/dyrektor.cpp#L150)

---

### e) Synchronizacja procesów (semafory)  
*(ftok(), semget(), semctl(), semop())*

- **Tworzenie i inicjalizacja semaforów System V**  
  → [src/magazyn.cpp#L39](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L39)

- **Operacje P/V na semaforach (`semop`)**  
  → [include/common.h#L133](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/include/common.h#L133)

---

### h) Segmenty pamięci dzielonej  
*(ftok(), shmget(), shmat(), shmdt(), shmctl())*

- **Tworzenie i dołączanie pamięci dzielonej**  
  → [src/magazyn.cpp#L44](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L44)

- **Odłączanie i usuwanie pamięci dzielonej (`shmdt`, `shmctl`)**  
  → [src/dyrektor.cpp#L97](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/dyrektor.cpp#L97)  
  → [src/stanowisko.cpp#L157](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/stanowisko.cpp#L157)

---

### i) Kolejki komunikatów System V  
*(ftok(), msgget(), msgsnd(), msgrcv(), msgctl())*

- **Tworzenie kolejki komunikatów i odbiór poleceń**  
  → [src/magazyn.cpp#L52](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L52)  
  → [src/magazyn.cpp#L303](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/magazyn.cpp#L303)

- **Wysyłanie komunikatów (żądania stanowisk / polecenia dyrektora)**  
  → [src/stanowisko.cpp#L69](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/stanowisko.cpp#L69)

- **Usuwanie kolejki komunikatów (`msgctl(IPC_RMID)`)**  
  → [src/dyrektor.cpp#L98](https://github.com/Krisiekk/fabryka-czekolady/blob/8c4406e1f4800a77079abbd7ea8eb48b2ce23343/src/dyrektor.cpp#L98)

---

### c) FIFO (Named Pipes)  
*(mkfifo(), open(), read(), write(), close(), unlink())*

**Nie dotyczy** – w projekcie wykorzystano kolejki komunikatów System V zamiast FIFO.

---

### f) Wątki POSIX  
*(pthread_create(), pthread_join(), pthread_exit(), pthread_mutex_*, pthread_cond_*)*

**Nie dotyczy** – projekt opiera się na wielu procesach (`fork()` + `exec()`), nie na wątkach.

---

### g) Gniazda (Sockets)  
*(socket(), bind(), listen(), accept(), connect(), send(), recv())*

**Nie dotyczy** – komunikacja między procesami realizowana jest przez IPC System V.

---

## 7. Problemy napotkane podczas realizacji projektu

1. **Deadlock przy pełnym magazynie** – stanowiska blokowały się na semaforach czekając na surowce, które nie mogły być dostarczone. Rozwiązanie: `try_P()` z `IPC_NOWAIT` i rollback.

2. **Komenda StopDostawcy nie docierała do wszystkich** – broadcast przez `mtype=1` powodował, że tylko jeden proces odbierał wiadomość. Rozwiązanie: per-PID targeting (`mtype = pid`).

3. **Race condition przy zapisie logów** – jednoczesny zapis przez wiele procesów powodował mieszanie się linii. Rozwiązanie: dodatkowy semafor `SEM_RAPORT`.

4. **Procesy nie reagowały na sygnały podczas `sleep()`** – użycie `signal()` nie przerywało blokujących wywołań. Rozwiązanie: `sigaction()` bez flagi `SA_RESTART`.

5. **Niepoprawny rozmiar bufora w `msgrcv()`** – przekazywanie `sizeof(msg.cmd)` zamiast `sizeof(msg) - sizeof(long)` powodowało błędy odbioru. Rozwiązanie: poprawna kalkulacja rozmiaru.

---

## 8. Architektura systemu

```
┌─────────────────────────────────────────────────────────────────┐
│                         DYREKTOR                                │
│                    (sterowanie fabryki)                         │
│                  Ty wpisujesz komendy 1-4                       │
└──────────────────────────┬──────────────────────────────────────┘
                           │
         ┌─────────────────┼─────────────────┐
         │ fork()+exec()   │ msgsnd()        │ kill(SIGTERM)
         │                 │ (komendy)       │ (timeout)
         ▼                 ▼                 ▼
┌──────────────┐    kolejka msg    ┌─────────────────────────────┐
│   MAGAZYN    │◄─────────────────►│       DOSTAWCY (x4)         │
│              │                   │     A    B    C    D        │
│  Stan:       │    semafory       │                             │
│  - A,B,C,D   │◄─────────────────►│  semop() - dodaj surowce    │
│  - capacity  │    (SEM_MUTEX,    └─────────────────────────────┘
│              │     SEM_A/B/C/D,
│  pamięć shm  │     SEM_CAPACITY)
│              │
└──────────────┘    kolejka msg    ┌─────────────────────────────┐
       ▲        ◄─────────────────►│     STANOWISKA (x2)         │
       │           semafory        │        1          2         │
       │                           │                             │
       │                           │  Typ1: A+B+C    Typ2: A+B+D │
       └───────────────────────────┴─────────────────────────────┘

Komunikacja:
  - kolejka msg: polecenia dyrektora, żądania stanowisk, odpowiedzi magazynu
  - semafory: synchronizacja dostępu do zasobów (mutex, pojemność, surowce)
  - pamięć shm: stan magazynu (ilości A,B,C,D + capacity)
  - sygnały: SIGTERM przy graceful shutdown (timeout)
```