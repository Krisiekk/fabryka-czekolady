# Fabryka czekolady

Projekt zrealizowany w ramach przedmiotu **Systemy Operacyjne**.  
Symulacja problemu **Producent–Konsument** z wykorzystaniem procesów oraz
mechanizmów IPC systemu UNIX.

---

## Opis projektu

System symuluje pracę fabryki produkującej dwa rodzaje czekolady:

- **Stanowisko 1** – wykorzystuje składniki: A, B, C  
- **Stanowisko 2** – wykorzystuje składniki: A, B, D  

Składniki są przechowywane w magazynie o pojemności **N jednostek**, gdzie:
- A, B → 1 jednostka
- C → 2 jednostki
- D → 3 jednostki

W systemie działają następujące procesy:
- **dyrektor** – uruchamia system i steruje jego zakończeniem,
- **magazyn** – zarządza stanem magazynu i synchronizacją dostępu,
- **4 dostawców** (A, B, C, D) – dostarczają surowce w losowych momentach,
- **2 stanowiska produkcyjne** – produkują czekoladę.

Procesy komunikują się przy użyciu:
- semaforów systemowych (19 semaforów),
- pamięci dzielonej (ring buffer z offsetami w semaforach),
- sygnałów systemowych (SIGTERM, SIGUSR1).

Przebieg symulacji zapisywany jest do pliku tekstowego, a stan magazynu
jest zapisywany przy zamykaniu systemu (komenda StopAll) i odtwarzany przy ponownym uruchomieniu.

---

## Struktura projektu

- `dyrektor` – uruchamianie systemu, menu sterujące, graceful shutdown  
- `magazyn` – obsługa magazynu, synchronizacja, zapis/odczyt stanu  
- `dostawca` – procesy dostawców A/B/C/D  
- `stanowisko` – procesy stanowisk produkcyjnych  
- `common.h` – wspólne definicje i funkcje pomocnicze  

Pliki generowane w trakcie działania:
- `raport.txt` – raport z przebiegu symulacji
- `magazyn_state.txt` – zapisany stan magazynu

---

## Kompilacja

Projekt wykorzystuje **CMake**.

```bash
rm -rf build
mkdir build
cd build
cmake ..
make
cd build
./dyrektor <N>
# przykład:
./dyrektor 100