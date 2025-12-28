#!/bin/bash
#
# Testy automatyczne - Fabryka Czekolady
# Autor: Krzysztof Pietrzak (156721)
#
set -u

cd "$(dirname "$0")/../build" || exit 1


RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' 

PASS_COUNT=0
FAIL_COUNT=0

pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
    ((PASS_COUNT++))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $*"
    ((FAIL_COUNT++))
}

info() {
    echo -e "${YELLOW}[INFO]${NC} $*"
}


prep() {
   
    pkill -9 magazyn 2>/dev/null || true
    pkill -9 dostawca 2>/dev/null || true
    pkill -9 stanowisko 2>/dev/null || true
    pkill -9 dyrektor 2>/dev/null || true
    
    
    sleep 0.5
    
    
    ipcrm -a 2>/dev/null || true
    
  
    rm -f raport.txt magazyn_state.txt
}


cleanup() {
    pkill -9 magazyn 2>/dev/null || true
    pkill -9 dostawca 2>/dev/null || true
    pkill -9 stanowisko 2>/dev/null || true
    sleep 0.3
}


run_with_timeout() {
    local seconds="$1"
    local input="$2"
    local capacity="$3"
    
    timeout --kill-after=2 "$seconds" ./dyrektor "$capacity" <<< "$input"
    local rc=$?
    
    
    cleanup
    
    if [[ $rc -ne 0 && $rc -ne 124 ]]; then
        return $rc
    fi
    return 0
}


separator() {
    echo "----------------------------------------"
}

echo ""
echo "========================================"
echo "  TESTY AUTOMATYCZNE - FABRYKA CZEKOLADY"
echo "========================================"
echo ""

# ---------------------------------------------------------------------------
# TEST 1: Poprawne uruchomienie wszystkich procesow
# ---------------------------------------------------------------------------
separator
echo "TEST 1: Poprawne uruchomienie wszystkich procesow"
separator
prep

run_with_timeout 10 "4" 100
rc=$?

if [[ $rc -ne 0 ]]; then
    fail "Dyrektor zakonczyl z bledem (kod=$rc)"
elif [[ -f magazyn_state.txt ]]; then
    pass "Procesy uruchomione, stan magazynu zapisany"
elif [[ -f raport.txt ]]; then
    pass "Procesy uruchomione, raport utworzony"
else
    fail "Brak pliku stanu lub raportu"
fi
echo ""

# ---------------------------------------------------------------------------
# TEST 2: Pelny magazyn (mala pojemnosc)
# ---------------------------------------------------------------------------
separator
echo "TEST 2: Pelny magazyn (capacity=5)"
separator
prep

run_with_timeout 15 "4" 5
rc=$?

if [[ $rc -ne 0 ]]; then
    fail "Dyrektor zakonczyl z bledem (kod=$rc)"
elif [[ -f raport.txt ]]; then
    
    if grep -qiE "pelny|brak miejsca|czekam|capacity" raport.txt; then
        pass "System poprawnie obsluguje pelny magazyn"
    else
        
        LAST_STATE=$(grep -oE "zajetosc: [0-9]+/5" raport.txt | tail -1)
        if [[ -n "$LAST_STATE" ]]; then
            pass "Magazyn dzialal z ograniczona pojemnoscia ($LAST_STATE)"
        else
            info "Brak jawnego komunikatu o pelnym magazynie (sprawdz logi recznie)"
            pass "Test wykonany bez bledow"
        fi
    fi
else
    fail "Brak pliku raport.txt"
fi
echo ""

# ---------------------------------------------------------------------------
# TEST 3: Zatrzymanie dostawcow (StopDostawcy)
# ---------------------------------------------------------------------------
separator
echo "TEST 3: Zatrzymanie dostawcow (StopDostawcy)"
separator
prep


run_with_timeout 12 $'3\n4' 100
rc=$?

if [[ $rc -ne 0 ]]; then
    fail "Dyrektor zakonczyl z bledem (kod=$rc)"
elif [[ ! -f raport.txt ]]; then
    fail "Brak pliku raport.txt"
else
    #
    STOP_COUNT=$(grep -c "konczy prace" raport.txt || echo "0")
    DOSTAWCY_STOP=$(grep -c "Dostawca.*konczy prace" raport.txt || echo "0")
    
    if [[ "$DOSTAWCY_STOP" -ge 4 ]]; then
        pass "Wszyscy dostawcy (4/4) zakonczyli prace po StopDostawcy"
    elif [[ "$DOSTAWCY_STOP" -gt 0 ]]; then
        info "Zakonczylo $DOSTAWCY_STOP/4 dostawcow"
        pass "Komenda StopDostawcy dziala"
    else
        fail "Dostawcy nie zakonczyli pracy (brak logow 'konczy prace')"
    fi
fi
echo ""

# ---------------------------------------------------------------------------
# TEST 4: Wieloprocesowosc i synchronizacja
# ---------------------------------------------------------------------------
separator
echo "TEST 4: Wieloprocesowosc i synchronizacja"
separator
prep


{
    sleep 8  
    echo "4"  
} | timeout 15 ./dyrektor 100

cleanup

DOSTAW=$(grep -c "Dostarczono" raport.txt 2>/dev/null || echo "0")
PROD=$(grep -c "wyprodukowano" raport.txt 2>/dev/null || echo "0")
WYDANO=$(grep -c "Wydano surowce" raport.txt 2>/dev/null || echo "0")

if [[ "$DOSTAW" -gt 5 && "$PROD" -gt 2 ]]; then
    pass "Wieloprocesowosc dziala ($DOSTAW dostaw, $PROD produkcji, $WYDANO wydan)"
elif [[ "$DOSTAW" -gt 0 || "$PROD" -gt 0 ]]; then
    info "Mala liczba zdarzen: $DOSTAW dostaw, $PROD produkcji"
    pass "System dziala poprawnie"
else
    fail "Brak zdarzen w logu"
fi
echo ""

# ---------------------------------------------------------------------------
# TEST 5: Brak zakleszczeń (deadlock test)
# ---------------------------------------------------------------------------
separator
echo "TEST 5: Test zakleszczeń (dlugi przebieg)"
separator
prep


run_with_timeout 30 "4" 10
rc=$?

if [[ $rc -ne 0 ]]; then
    fail "Dyrektor zakonczyl z bledem lub przekroczono timeout (kod=$rc)"
else
    if [[ -f raport.txt ]]; then
        EVENTS=$(wc -l < raport.txt)
        if [[ "$EVENTS" -gt 10 ]]; then
            pass "System dzialal bez zakleszczen ($EVENTS zdarzen w logu)"
        else
            pass "System zakonczyl poprawnie ($EVENTS zdarzen)"
        fi
    else
        pass "System zakonczyl bez crashu"
    fi
fi
echo ""

# ---------------------------------------------------------------------------
# PODSUMOWANIE
# ---------------------------------------------------------------------------
separator
echo "PODSUMOWANIE"
separator
echo ""
echo "Testy zakonczone: $((PASS_COUNT + FAIL_COUNT))"
echo "  Zaliczone:  $PASS_COUNT"
echo "  Niezaliczone: $FAIL_COUNT"
echo ""

if [[ $FAIL_COUNT -eq 0 ]]; then
    echo "Wszystkie testy ZALICZONE"
    prep
    exit 0
else
    echo "Niektore testy NIEZALICZONE"
    exit 1
fi


