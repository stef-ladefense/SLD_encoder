# SLDencodeur (V1.05)

**SLDencodeur** est une bibliothèque ultra-légère, performante et industrielle pour la gestion des encodeurs rotatifs sur ESP32/ESP8266. Elle utilise une machine à états finis (FSM) pour un décodage matériel via interruptions, garantissant une réactivité maximale sans aucun rebond logiciel, même sous forte charge CPU (WiFi/Audio).

---

## 🚀 Installation & Configuration

### 1. Déclaration Globale (Constructeur Dynamique)
Le choix du moteur de décodage se fait lors de l'instanciation. Plus besoin de modifier la bibliothèque pour s'adapter à votre matériel.

```cpp
#include <SLD_encoder.hpp>

#define PIN_A 5
#define PIN_B 4

// MODE_BUXTON_FULL (Défaut) : Immunité totale au bruit, 1 impulsion par "clic" physique.
SLDencodeur Encoder(PIN_A, PIN_B); 

// Autres configurations possibles :
// SLDencodeur Encoder(PIN_A, PIN_B, MODE_BUXTON_HALF); // 2 impulsions par cycle (00 et 11)
// SLDencodeur Encoder(PIN_A, PIN_B, MODE_STANDARD, 4);  // Moteur x4 (4 micro-pas par cran)
```

### 2. Routine d'Interruption (ISR)
Vous devez déclarer une fonction d'interruption qui appelle la méthode `tick()`. Pour une performance optimale sur ESP32, placez-la impérativement dans l'IRAM.

```cpp
void IRAM_ATTR encodeurISR() {
    Encoder.tick();
}
```

### 3. Initialisation (setup)
Dans votre `setup()`, initialisez l'objet (configure les pins en INPUT_PULLUP) et attachez les interruptions sur les deux pins (mode `CHANGE`) :

```cpp
void setup() {
    Encoder.begin(); 
    attachInterrupt(digitalPinToInterrupt(PIN_A), encodeurISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_B), encodeurISR, CHANGE);
}
```

---

## 🧠 Fonctionnement d'un Encodeur Rotatif

Un encodeur rotatif mécanique émet un signal sur deux sorties (A et B) en décalage de phase (quadrature). Ce signal suit une séquence précise appelée **Code de Gray**, où un seul bit change à chaque transition.

*   **Sens Horaire (CW)** : `00` → `01` → `11` → `10` → `00`
*   **Sens Anti-Horaire (CCW)** : `00` → `10` → `11` → `01` → `00`

---

## 🏎️ Le Moteur MODE_STANDARD (4-bits)

Ce moteur est conçu pour la précision et la réactivité extrême. Il détecte les 4 changements d'état électrique d'un cycle complet.

### 1. La construction de l'Index
Pour déterminer le sens du mouvement, on combine l'**Ancien état** et le **Nouvel état** pour créer un index de 0 à 15 (4 bits) : `Index = (AncienA AncienB) + (NouveauA NouveauB)`.

### 2. Analyse de la Table de Vérité Standard
Voici le contenu détaillé du filtrage pour chaque transition possible :

```text
┌───────┬────────────────────┬────────┬────────┬─────────────────────────────────────┐
│ Index │ Binaire (Old->New) │ Action │ Valeur │ Explication                         │
├───────┼────────────────────┼────────┼────────┼─────────────────────────────────────┤
│ 0     │ 0000               │ Rien   │ 0      │ Pas de mouvement                    │
│ 1     │ 0001               │ CW     │ +1     │ Transition 00 → 01                  │
│ 2     │ 0010               │ CCW    │ -1     │ Transition 00 → 10                  │
│ 3     │ 0011               │ Erreur │ 0      │ Saut impossible (Bruit électrique)  │
│ 4     │ 0100               │ CCW    │ -1     │ Transition 01 → 00                  │
│ 5     │ 0101               │ Rien   │ 0      │ Pas de mouvement                    │
│ 6     │ 0110               │ Erreur │ 0      │ Saut impossible                     │
│ 7     │ 0111               │ CW     │ +1     │ Transition 01 → 11                  │
│ 8     │ 1000               │ CW     │ +1     │ Transition 10 → 00                  │
│ 9     │ 1001               │ Erreur │ 0      │ Saut impossible                     │
│ 10    │ 1010               │ Rien   │ 0      │ Pas de mouvement                    │
│ 11    │ 1011               │ CCW    │ -1     │ Transition 10 → 11                  │
│ 12    │ 1100               │ Erreur │ 0      │ Saut impossible                     │
│ 13    │ 1101               │ CCW    │ -1     │ Transition 11 → 01                  │
│ 14    │ 1110               │ CW     │ +1     │ Transition 11 → 10                  │
│ 15    │ 1111               │ Rien   │ 0      │ Pas de mouvement                    │
└───────┴────────────────────┴────────┴────────┴─────────────────────────────────────┘
```

### 3. La résolution (Division par 4)
Un cycle complet (un "clic" physique) passe par 4 transitions de la table. Le compteur interne atteint donc +/- 4 par clic. La fonction `getDeltaBrut()` divise cette valeur par le paramètre `stepsPerClick` (ex: 4) pour renvoyer un mouvement unitaire.

### 4. Pourquoi c'est magique contre les rebonds ?
Imaginez un parasite (bruit) qui fait osciller la Pin A : `00` → `01` → `00` → `01`.
1.  `00` → `01` (Index 1) : La table dit **+1**.
2.  `01` → `00` (Index 4) : La table dit **-1**.
3.  `00` → `01` (Index 1) : La table dit **+1**.
Le compteur fait : $0 + 1 - 1 + 1 = \mathbf{1}$. Le bruit s'est auto-annulé mathématiquement.

---

## 🛡️ Les Moteurs MODE_BUXTON (Automate FSM)

Les modes Buxton sont destinés aux encodeurs mécaniques usés, bas de gamme ou très bruités. Au lieu de compter chaque flanc, ils valident un **cheminement** à travers un graphe d'états.

### 1. Le Principe du Graphe
L'automate interne possède 7 états. Il ne valide un changement de compteur **QUE** s'il a vu la séquence complète et ordonnée du code Gray. 

**Pourquoi Buxton est invulnérable au bruit ?**
Si l'encodeur "grésille" (ex: oscille entre 00 et 01), l'automate bascule entre l'état `R_START` et `R_CW_BEGIN`. Tant qu'il n'a pas reçu le signal suivant (`11`), il reste bloqué dans cet entre-deux et n'émet **aucun signal**. Le bruit est littéralement prisonnier de la logique.

### 2. La Matrice de Transition (Full-Step)
L'automate reste dans un état (ligne) et attend un signal électrique (colonne) pour transiter.

```text
┌───────────────┬──────┬──────┬──────┬──────┬───────────────────────────────────┐
│ État Actuel   │ 00   │ 01   │ 10   │ 11   │ Résultat / Action                 │
├───────────────┼──────┼──────┼──────┼──────┼───────────────────────────────────┤
│ R_START (0)   │ Start│ CW_B │ CCW_B│ Start│ Point de repos (Repos)            │
│ R_CW_FINAL (1)│ Next │ Start│ CW_F │ Emit │ Émet DIR_CCW si retour à 11       │
│ R_CW_BEGIN (2)│ Next │ CW_B │ Start│ Start│ Début de rotation horaire         │
│ R_CW_NEXT (3) │ Next │ CW_B │ CW_F │ Start│ Transition intermédiaire          │
│ R_CCW_BEGIN(4)│ Next │ Start│ CCW_B│ Start│ Début de rotation anti-horaire    │
│ R_CCW_FINAL(5)│ Next │ CCW_F│ Start│ Emit │ Émet DIR_CW si retour à 11        │
│ R_CCW_NEXT (6)│ Next │ CCW_F│ CCW_B│ Start│ Transition intermédiaire          │
└───────────────┴──────┴──────┴──────┴──────┴───────────────────────────────────┘
```

### 3. Le mode HALF_STEP
Le mode `MODE_BUXTON_HALF` utilise une table simplifiée qui valide un mouvement à chaque point de passage `00` et `11`. C'est le compromis idéal pour doubler la résolution tout en gardant le filtrage Buxton.

---

## 🛠️ API & Utilisation Avancée

### Lecture de la direction (Navigation)
`getDelta()` : Retourne **-1** (CCW), **0** ou **+1** (CW). Idéal pour naviguer dans des menus.

### Lecture brute (Comptage)
`getDeltaBrut()` : Retourne le nombre réel de crans parcourus.

### Lecture avec accélération (Vélocité)
`getAcceleratedDelta()` : Multiplie le mouvement si les impulsions sont rapprochées. Indispensable pour le Volume ou l'Égaliseur.
- **Seuil < 25ms** : Multiplicateur **x4**.
- **Seuil < 60ms** : Multiplicateur **x2**.

---

## ✨ Avantages Techniques de la V1.05
- **Zéro Délai** : Aucun `delay()` ni `millis()` bloquant pour le déparasitage.
- **DRAM Compliance** : Tables de vérité forcées en RAM statique pour éviter les crashs liés au cache Flash sur ESP32.
- **Protection Atomique** : Utilisation de `noInterrupts()` lors de la lecture pour garantir l'intégrité des données sous forte charge.
- **C++20 Ready** : Syntaxe `count = count + 1` pour éviter les avertissements sur les variables `volatile`.
- **Always Inline** : Injection du code `tick()` directement dans l'ISR pour une performance maximale.

---

## 📜 Crédits & Remerciements
L'algorithme de Machine à États (FSM) des modes Buxton est basé sur le travail exceptionnel de **Ben Buxton** (2011). Considéré par la communauté comme **l'ultime façon de gérer les encodeurs**, son approche a sauvé d'innombrables projets.
- **Blog** : [buxtronix.net](http://www.buxtronix.net/2011/10/rotary-encoders-done-properly.html)
- **GitHub** : [buxtronix/arduino/Rotary](https://github.com/buxtronix/arduino/tree/master/libraries/Rotary)

---
*(C) 2026 Stef_ladéfense - Version 1.05*
V1.00 Première Version (SB_encodeur).
V1.01 Ajout de l'accélération temporelle.
V1.02 Ajout de getDeltaBrut et normalisation de getDelta (+/- 1).
V1.03 Migration SLD, Optimisation ESP32 (DRAM/Atomic).
V1.04 Dual-Engine (Standard vs Buxton).
V1.05 Triple-Engine, Constructeur Dynamique, Support C++20.
