#ifndef SLD_ENCODER_H
#define SLD_ENCODER_H

// ============================================================================================
// HISTORIQUE DES VERSIONS (SLD_encoder)
// ============================================================================================
// Ver 1.00 : 05/01/2026 - Première version (SB_encodeur).
// Ver 1.01 : 06/01/2026 - Ajout de getAcceleratedDelta.
// Ver 1.02 : 07/01/2026 - Ajout de getDeltaBrut et normalisation de getDelta (+/- 1).
// Ver 1.03 : 27/02/2026 - Migration SLD. Version 4-bits Robuste optimisée ESP32.
// Ver 1.04 : 27/02/2026 - Dual-Engine : Mode 4-bits Standard vs Mode Buxton (State Machine).
// Ver 1.05 : 27/02/2026 - Configuration dynamique via constructeur (Triple-Engine).
//                         - Ajout de l'enum EncoderMode.
//                         - Valeurs par défaut : MODE_BUXTON_FULL, steps = 1.
//                         - Inlining forcé et protection atomique ESP32.
// ============================================================================================

/**
 * SLD_encoder - Bibliothèque d'encodeur rotatif ultra-légère optimisée pour ESP32.
 * (C) 2026 Stef_ladéfense - Version 1.05
 * 
 * Basée sur l'algorithme de Ben Buxton pour les modes BUXTON_FULL et BUXTON_HALF.
 */

#include <Arduino.h>

/**
 * @enum EncoderMode
 * @brief Sélection du moteur de décodage au moment de l'instanciation.
 */
/**
 * @enum EncoderMode
 * @brief Définit le comportement du moteur de décodage.
 */
enum EncoderMode
{
    MODE_STANDARD,    // Moteur x4 : détecte chaque changement électrique (4 par cran).
    MODE_BUXTON_FULL, // Moteur Buxton : 1 pas validé par cycle complet (ultra-stable).
    MODE_BUXTON_HALF  // Moteur Buxton : 1 pas validé tous les demi-cycles (00 et 11).
};

// Signatures des bits de direction pour les tables Buxton
#define R_START 0x0
#define DIR_CW 0x10  // Sens horaire
#define DIR_CCW 0x20 // Sens anti-horaire

/**
 * TABLES DE VÉRITÉ (DRAM_ATTR force le placement en RAM statique pour éviter les crashs IRAM)
 */

// Table Standard 4-bits : Retourne le delta immédiat (+1, -1 ou 0) selon l'index [OldAB][NewAB]
static const int8_t SLD_STD_STATES[] DRAM_ATTR = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};

// Table Buxton Full-Step : Graphe d'états (7 lignes). Ne déclenche DIR_CW/DIR_CCW qu'à la fin d'un cycle complet.
static const uint8_t SLD_BT_FULL[7][4] DRAM_ATTR = {
    {0, 2, 4, 0}, {3, 0, 1, 0 | DIR_CCW}, {3, 2, 0, 0}, {3, 2, 1, 0}, {6, 0, 4, 0}, {6, 5, 0, 0 | DIR_CW}, {6, 5, 4, 0}};

// Table Buxton Half-Step : Graphe d'états (6 lignes). Déclenche DIR_CW/DIR_CCW aux points de passage 00 et 11.
static const uint8_t SLD_BT_HALF[6][4] DRAM_ATTR = {
    {3, 2, 1, 0}, {3 | DIR_CW, 0, 1, 0}, {3 | DIR_CCW, 2, 0, 0}, {3, 5, 4, 0}, {3, 3, 4, 0 | DIR_CCW}, {3, 5, 3, 0 | DIR_CW}};

/**
 * @class SLDencodeur
 * @brief Version Triple-Engine optimisée pour ESP32.
 */
class SLDencodeur
{
public:
    /**
     * @brief Constructeur flexible.
     * @param pinA Pin du signal A
     * @param pinB Pin du signal B
     * @param mode MODE_BUXTON_FULL par défaut pour une stabilité maximale.
     * @param stepsPerClick 1 par défaut pour Buxton, 4 conseillé pour Standard.
     */
    SLDencodeur(uint8_t pinA, uint8_t pinB, EncoderMode mode = MODE_BUXTON_FULL, uint8_t stepsPerClick = 1)
        : _pinA(pinA), _pinB(pinB), _steps(stepsPerClick), _mode(mode), _old_AB(0), _state(R_START)
    {
        // Ajustement automatique des pas par clic si on est en mode Buxton (1 cran = 1 increment)
        if (_mode != MODE_STANDARD)
            _steps = 1;
    }

    void begin()
    {
        pinMode(_pinA, INPUT_PULLUP);
        pinMode(_pinB, INPUT_PULLUP);
        _old_AB = (digitalRead(_pinB) << 1) | digitalRead(_pinA);
        _state = R_START;
    }

    /**
     * @brief Routine de traitement du signal (à appeler depuis l'ISR)
     * L'attribut always_inline injecte ce code directement dans l'ISR pour éviter les erreurs de lien Xtensa.
     */
    inline void __attribute__((always_inline)) tick(bool isPinB = false)
    {
        // Lecture rapide de l'état actuel des pins
        uint8_t AB = (digitalRead(_pinB) << 1) | digitalRead(_pinA);

        if (_mode == MODE_STANDARD)
        {
            // Logique 4-bits classique
            if (AB != _old_AB)
            {
                // On récupère le delta dans la table via l'index construit sur 4 bits [Old][New]
                _count = _count + SLD_STD_STATES[((_old_AB << 2) | AB) & 0x0F];
                _old_AB = AB; // Mémorise le nouvel état
            }
        }
        else
        {
            // Logique Buxton (Machine à États)
            // On récupère le résultat (nouvel état + flag de direction éventuel) dans la table Buxton choisie
            uint8_t res = (_mode == MODE_BUXTON_FULL) ? SLD_BT_FULL[_state & 0x0F][AB] : SLD_BT_HALF[_state & 0x0F][AB];
            _state = res & 0x0F; // Extrait l'index du nouvel état de l'automate (4 bits de poids faible)

            // Si le résultat contient un bit de direction (DIR_CW ou DIR_CCW), on met à jour le compteur
            if (res & DIR_CW)
                _count = _count + 1;
            else if (res & DIR_CCW)
                _count = _count - 1;
        }
    }

    /**
     * @brief Retourne le nombre de crans parcourus (dividende par stepsPerClick)
     * @return int8_t Nombre de crans (+X, -X) ou 0 si aucun pas complet n'est validé.
     */
    int8_t getDeltaBrut()
    {
        noInterrupts(); // Désactive les interruptions pour éviter un conflit avec tick() pendant la lecture
        int8_t val = _count;
        if (abs(val) >= _steps)
        {
            _count = 0;          // Réinitialise le compteur après validation d'un pas complet
            interrupts();        // Réactive les interruptions
            return val / _steps; // Retourne le nombre de crans réels (ex: +1, -2)
        }
        interrupts();
        return 0; // Pas assez de micro-pas parcourus pour valider un cran
    }

    /**
     * @brief Retourne uniquement la direction normalisée (-1, 0, 1)
     */
    int8_t getDelta()
    {
        int8_t val = getDeltaBrut();
        return (val > 0) ? 1 : ((val < 0) ? -1 : 0);
    }

    /**
     * @brief Calcule le delta avec accélération temporelle
     * @return int8_t Delta multiplié selon la vitesse de rotation.
     */
    int8_t getAcceleratedDelta()
    {
        int8_t delta = getDeltaBrut(); // Récupère le mouvement de base
        if (delta != 0)
        {
            unsigned long now = millis();
            unsigned long dt = now - _lastTime; // Temps écoulé depuis le dernier mouvement validé
            _lastTime = now;

            // Si le mouvement est très rapide (< 20ms), on multiplie par 4
            if (dt < 25)
                return delta * 4;
            // Si le mouvement est rapide (< 50ms), on multiplie par 2
            if (dt < 60)
                return delta * 2;
        }
        return delta; // Retourne le mouvement sans accélération sinon
    }

    void reset()
    {
        noInterrupts();
        _count = 0;
        _state = R_START;
        interrupts();
    }

private:
    uint8_t _pinA, _pinB, _steps, _old_AB, _state;
    EncoderMode _mode;
    volatile int8_t _count = 0;
    unsigned long _lastTime = 0;
};

#endif
