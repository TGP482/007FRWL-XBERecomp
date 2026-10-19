// Text the recomp adds in French, German, Italian and Spanish: Double Agent recomp's wording where the English is the
// same, else the game's own words where its text has them, in sentence case with German's nouns capitalized; the
// panel's rows and the game's menu show it in capitals.
#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <mutex>

#include "common/options.h"
#include "common/translations.h"

namespace {

// in game::kLanguages order after int
struct Entry {
    std::string_view english;
    std::u16string_view text[4];
};
constexpr Entry kEntries[] = {
    // options menu pages and the RECOMP SETTINGS sections
    {"RECOMP SETTINGS", {u"Paramètres Recomp", u"Recomp-Einstellungen", u"Impostazioni Recomp", u"Configuración de Recomp"}},
    {"MOUSE AND KEYBOARD", {u"Souris et clavier", u"Maus und Tastatur", u"Mouse e tastiera", u"Ratón y teclado"}},
    {"CONTROLLER", {u"Manette", u"Controller", u"Controller", u"Mando"}},
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos"}},
    {"RESTORE DEFAULTS", {u"Rétablir les valeurs par défaut", u"Standard wiederherstellen", u"Ripristina predefiniti", u"Restaurar predeterminados"}},
    {"PRESS A KEY", {u"Appuyez sur une touche", u"Taste drücken", u"Premi un tasto", u"Pulsa una tecla"}},
    {"PRESS A BUTTON", {u"Appuyez sur un bouton", u"Taste drücken", u"Premi un pulsante", u"Pulsa un botón"}},
    {"UNBOUND", {u"Non assigné", u"Nicht belegt", u"Non assegnato", u"Sin asignar"}},

    // RECOMP SETTINGS rows
    {"MONITOR", {u"Écran", u"Monitor", u"Monitor", u"Monitor"}},
    {"DISPLAY MODE", {u"Mode d'affichage", u"Anzeigemodus", u"Modalità schermo", u"Modo de pantalla"}},
    {"RESOLUTION", {u"Résolution", u"Auflösung", u"Risoluzione", u"Resolución"}},
    {"RENDER RESOLUTION", {u"Résolution de rendu", u"Render-Auflösung", u"Risoluzione di rendering", u"Resolución de renderizado"}},
    {"FIELD OF VIEW", {u"Champ de vision", u"Sichtfeld", u"Campo visivo", u"Campo de visión"}},
    {"VERTICAL SYNC", {u"Synchro verticale", u"V-Sync", u"Sincronia verticale", u"Sincronismo vertical"}},
    {"FMV SCALE", {u"Échelle des cinématiques", u"Videoskalierung", u"Scala dei filmati", u"Escala de vídeos"}},
    {"FILL", {u"Remplir", u"Füllen", u"Riempi", u"Rellenar"}},
    {"HDR OUTPUT", {u"Sortie HDR", u"HDR-Ausgabe", u"Uscita HDR", u"Salida HDR"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx."}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico"}},
    {"DRAW DISTANCE", {u"Distance d'affichage", u"Sichtweite", u"Distanza visiva", u"Distancia de dibujado"}},
    {"AMBIENT OCCLUSION", {u"Occlusion ambiante", u"Ambient Occlusion", u"Occlusione ambientale", u"Oclusión ambiental"}},
    {"SKIP INTROS", {u"Passer les intros", u"Intros überspringen", u"Salta introduzione", u"Saltar introducción"}},
    {"SKIPPABLE CUTSCENES", {u"Cinématiques passables", u"Überspringbare Zwischensequenzen", u"Filmati saltabili", u"Escenas saltables"}},
    {"PERSISTENT BODIES", {u"Corps persistants", u"Bleibende Leichen", u"Corpi persistenti", u"Cuerpos persistentes"}},
    {"PAUSE ON FOCUS LOSS", {u"Pause en arrière-plan", u"Pause im Hintergrund", u"Pausa in background", u"Pausa en segundo plano"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma"}},
    {"AUDIO OUTPUT", {u"Sortie audio", u"Audioausgabe", u"Uscita audio", u"Salida de audio"}},

    // their values
    {"FULLSCREEN", {u"Plein écran", u"Vollbild", u"Schermo intero", u"Pantalla completa"}},
    {"WINDOWED", {u"Fenêtré", u"Fenster", u"Finestra", u"Ventana"}},
    {"BORDERLESS", {u"Fenêtré sans bordure", u"Randloses Fenster", u"Finestra senza bordi", u"Ventana sin bordes"}},
    {"ON", {u"Oui", u"Ein", u"Sì", u"Sí"}},
    {"OFF", {u"Non", u"Aus", u"No", u"No"}},
    {"UNLIMITED", {u"Illimité", u"Unbegrenzt", u"Illimitato", u"Ilimitado"}},
    {"REFRESH RATE", {u"Fréquence de rafraîchissement", u"Bildwiederholfrequenz", u"Frequenza di aggiornamento", u"Frecuencia de actualización"}},
    {"AUTOMATIC", {u"Automatique", u"Automatisch", u"Automatico", u"Automático"}},
    {"GAME DEFAULT", {u"Par défaut du jeu", u"Spielstandard", u"Predefinito del gioco", u"Predeterminado del juego"}},
    {"NOT SUPPORTED", {u"Non pris en charge", u"Nicht unterstützt", u"Non supportato", u"No compatible"}},
    {"MONO", {u"Mono", u"Mono", u"Mono", u"Mono"}},
    {"2.0 STEREO", {u"2.0 Stéréo", u"2.0 Stereo", u"2.0 Stereo", u"2.0 Estéreo"}},
    {"5.1 SURROUND", {u"5.1 Surround", u"5.1 Surround", u"5.1 Surround", u"5.1 Envolvente"}},
    {"(DEFAULT)", {u"(par défaut)", u"(Standard)", u"(predefinito)", u"(por defecto)"}},
    {"(RESTART)", {u"(redémarrage)", u"(Neustart)", u"(riavvio)", u"(reinicio)"}},
    {"PRIMARY", {u"Principal", u"Primär", u"Principale", u"Principal"}},
    {"(PRIMARY)", {u"(principal)", u"(primär)", u"(principale)", u"(principal)"}},
    {"ENGLISH", {u"Anglais", u"Englisch", u"Inglese", u"Inglés"}},
    {"FRENCH", {u"Français", u"Französisch", u"Francese", u"Francés"}},
    {"GERMAN", {u"Allemand", u"Deutsch", u"Tedesco", u"Alemán"}},
    {"ITALIAN", {u"Italien", u"Italienisch", u"Italiano", u"Italiano"}},
    {"SPANISH", {u"Espagnol", u"Spanisch", u"Spagnolo", u"Español"}},

    // the cutscene skip prompt
    {"Skip", {u"Passer", u"Überspringen", u"Salta", u"Saltar"}},

    // MOUSE AND KEYBOARD rows
    {"MOUSE SENSITIVITY", {u"Sensibilité de la souris", u"Mausempfindlichkeit", u"Sensibilità del mouse", u"Sensibilidad del ratón"}},
    {"INVERT LOOK", {u"Inverser la visée", u"Blick umkehren", u"Inverti visuale", u"Invertir vista"}},
    {"CAMERA CENTRING", {u"Recentrage de la caméra", u"Kamerazentrierung", u"Centratura visuale", u"Centrado de cámara"}},
    {"FREE AIM", {u"Visée libre", u"Freies Zielen", u"Mira libera", u"Apuntado libre"}},
    {"WALK SPEED", {u"Vitesse de marche", u"Gehgeschwindigkeit", u"Velocità di camminata", u"Velocidad al caminar"}},
    {"MOVE FORWARD", {u"Avancer", u"Vorwärts bewegen", u"Avanti", u"Avanzar"}},
    {"MOVE BACK", {u"Reculer", u"Rückwärts bewegen", u"Indietro", u"Retroceder"}},
    {"STRAFE LEFT", {u"Pas latéral à gauche", u"Seitwärts nach links", u"Spostamento laterale a sinistra", u"Desplazamiento lateral a la izquierda"}},
    {"STRAFE RIGHT", {u"Pas latéral à droite", u"Seitwärts nach rechts", u"Spostamento laterale a destra", u"Desplazamiento lateral a la derecha"}},
    {"WALK", {u"Marcher", u"Gehen", u"Cammina", u"Caminar"}},
    {"CROUCH", {u"S'accroupir", u"Ducken", u"Chinati", u"Agacharse"}},
    {"JUMP", {u"Sauter", u"Springen", u"Salta", u"Saltar"}},
    {"CLIMB", {u"Grimper", u"Klettern", u"Sali", u"Trepar"}},
    {"DIVE ROLL", {u"Roulade", u"Hechtrolle", u"Capriola", u"Voltereta"}},
    {"FOCUS", {u"Mode Zoom", u"Bond-Fokus", u"Zoom di Bond", u"Enfoque Bond"}},
    {"WALL COVER", {u"S'abriter", u"In Deckung gehen", u"Copertura", u"Cubrirse"}},
    {"EXIT VEHICLE", {u"Sortir du véhicule", u"Fahrzeug verlassen", u"Scendi dal veicolo", u"Salir del vehículo"}},
    {"ITEM SELECT", {u"Sélectionner un objet", u"Objekt auswählen", u"Scegli un oggetto", u"Seleccionar un objeto"}},
    {"LOCK ON", {u"Verrouiller la cible", u"Ziel erfassen", u"Aggancia", u"Fijar objetivo"}},
    {"SCOPE ZOOM IN", {u"Zoom avant de la lunette", u"Zielfernrohr heranzoomen", u"Zoom avanti del mirino", u"Acercar la mira"}},
    {"SCOPE ZOOM OUT", {u"Zoom arrière de la lunette", u"Zielfernrohr herauszoomen", u"Zoom indietro del mirino", u"Alejar la mira"}},
    {"RESET CAMERA", {u"Recentrer la caméra", u"Kamera zurücksetzen", u"Ripristina visuale", u"Restablecer cámara"}},
    {"FIRE", {u"Tirer", u"Feuern", u"Fuoco", u"Disparar"}},
    {"RELOAD", {u"Recharger", u"Nachladen", u"Ricarica", u"Recargar"}},
    {"INTERACT", {u"Action", u"Aktion", u"Azione", u"Acción"}},
    {"OBJECTIVES", {u"Objectifs", u"Ziele", u"Obiettivi", u"Objetivos"}},
    {"PAUSE MENU", {u"Menu pause", u"Pausenmenü", u"Menu di pausa", u"Menú de pausa"}},
    {"TOGGLE HUD", {u"Afficher / masquer l'interface", u"HUD ein / aus", u"Mostra / nascondi HUD", u"Mostrar / ocultar HUD"}},
    {"SETTINGS PANEL", {u"Panneau des paramètres", u"Einstellungsmenü", u"Pannello impostazioni", u"Panel de configuración"}},
    {"LOOK UP", {u"Regarder en haut", u"Nach oben sehen", u"Guarda in alto", u"Mirar arriba"}},
    {"LOOK DOWN", {u"Regarder en bas", u"Nach unten sehen", u"Guarda in basso", u"Mirar abajo"}},
    {"LOOK LEFT", {u"Regarder à gauche", u"Nach links sehen", u"Guarda a sinistra", u"Mirar a la izquierda"}},
    {"LOOK RIGHT", {u"Regarder à droite", u"Nach rechts sehen", u"Guarda a destra", u"Mirar a la derecha"}},
    {"CAR", {u"Voiture", u"Auto", u"Auto", u"Coche"}},
    {"ACCELERATE", {u"Accélérer", u"Gas geben", u"Accelera", u"Acelerar"}},
    {"BRAKE / REVERSE", {u"Freiner / marche arrière", u"Bremsen / rückwärts", u"Frena / retromarcia", u"Frenar / marcha atrás"}},
    {"STEER LEFT", {u"Tourner à gauche", u"Nach links lenken", u"Sterza a sinistra", u"Girar a la izquierda"}},
    {"STEER RIGHT", {u"Tourner à droite", u"Nach rechts lenken", u"Sterza a destra", u"Girar a la derecha"}},
    {"HANDBRAKE", {u"Frein à main", u"Handbremse", u"Freno a mano", u"Freno de mano"}},
    {"FIRE WEAPON", {u"Tirer avec l'arme", u"Waffe abfeuern", u"Spara con l'arma", u"Disparar el arma"}},
    {"TARGET LOCK", {u"Verrouiller la cible", u"Ziel erfassen", u"Aggancia il bersaglio", u"Fijar objetivo"}},
    {"NEXT TARGET", {u"Cible suivante", u"Nächstes Ziel", u"Bersaglio successivo", u"Siguiente objetivo"}},
    {"CYCLE WEAPON", {u"Changer d'arme", u"Waffe wechseln", u"Cambia arma", u"Cambiar arma"}},
    {"USE GADGET", {u"Utiliser le gadget", u"Gadget benutzen", u"Usa il gadget", u"Usar el artilugio"}},
    {"LOOK BACK", {u"Regarder derrière", u"Nach hinten sehen", u"Guarda dietro", u"Mirar atrás"}},
    {"JETPACK AND Q COPTER", {u"Jetpack et Q-Copter", u"Jetpack und Q-Copter", u"Jetpack e Q-Copter", u"Jetpack y Q-Copter"}},
    {"FLY FORWARD", {u"Voler en avant", u"Vorwärts fliegen", u"Vola avanti", u"Volar hacia delante"}},
    {"FLY BACK", {u"Voler en arrière", u"Rückwärts fliegen", u"Vola indietro", u"Volar hacia atrás"}},
    {"FLY LEFT", {u"Voler à gauche", u"Nach links fliegen", u"Vola a sinistra", u"Volar a la izquierda"}},
    {"FLY RIGHT", {u"Voler à droite", u"Nach rechts fliegen", u"Vola a destra", u"Volar a la derecha"}},
    {"FLY UP", {u"Monter", u"Steigen", u"Sali", u"Subir"}},
    {"FLY DOWN", {u"Descendre", u"Sinken", u"Scendi", u"Bajar"}},
    {"LAND / EXIT", {u"Atterrir / quitter", u"Landen / verlassen", u"Atterra / esci", u"Aterrizar / salir"}},

    // the F10 panel's own
    {"SETTINGS", {u"Paramètres", u"Einstellungen", u"Impostazioni", u"Configuración"}},
    {"BACK", {u"Retour", u"Zurück", u"Indietro", u"Atrás"}},
    {"DEFAULTS", {u"Par défaut", u"Standard", u"Predefiniti", u"Por defecto"}},
    {"APPLY", {u"Appliquer", u"Übernehmen", u"Applica", u"Aplicar"}},
    {"MOUSE", {u"Souris", u"Maus", u"Mouse", u"Ratón"}},
    {"KEYBOARD", {u"Clavier", u"Tastatur", u"Tastiera", u"Teclado"}},
    {"INPUT DEVICES", {u"Périphériques", u"Eingabegeräte", u"Dispositivi di input", u"Dispositivos de entrada"}},
    {"PLAYER", {u"Joueur", u"Spieler", u"Giocatore", u"Jugador"}},
    {"NOT CONNECTED", {u"Non connecté", u"Nicht verbunden", u"Non collegato", u"No conectado"}},
    {"(UNPLUGGED)", {u"(débranché)", u"(entfernt)", u"(scollegato)", u"(desconectado)"}},
    {"Press a key or mouse button to bind, Escape cancels", {u"Appuyez sur une touche ou un bouton de souris, Échap pour annuler", u"Taste oder Maustaste drücken, Escape bricht ab", u"Premi un tasto o un pulsante del mouse, Esc per annullare", u"Pulsa una tecla o un botón del ratón, Escape para cancelar"}},
    {"Press a controller button to bind, Escape cancels", {u"Appuyez sur un bouton de la manette, Échap pour annuler", u"Controllertaste drücken, Escape bricht ab", u"Premi un pulsante del controller, Esc per annullare", u"Pulsa un botón del mando, Escape para cancelar"}},
    {"Right click or Delete clears the key", {u"Clic droit ou Suppr pour effacer la touche", u"Rechtsklick oder Entf löscht die Taste", u"Clic destro o Canc per cancellare il tasto", u"Clic derecho o Supr para borrar la tecla"}},
};

bool Same(std::string_view a, std::string_view b) {
    auto upper = [](char c) { return char(std::toupper(uint8_t(c))); };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return upper(x) == upper(y);
           });
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the language the game shows now, the one its boot screen set; -1 for English
int Language() {
    std::string_view shown = game::ShownLanguage();
    auto at = std::find(std::begin(game::kLanguages), std::end(game::kLanguages), shown);
    return at == std::end(game::kLanguages) ? -1 : int(at - std::begin(game::kLanguages)) - 1;
}

const char16_t* Translation(std::string_view english, int language) {
    for (const Entry& e : kEntries)
        if (Same(e.english, english)) return e.text[language].data();
    return nullptr;
}

// the longest phrase with a translation from text[at], at a word start; end set past it
const char16_t* PhraseAt(std::string_view text, size_t at, size_t& end, int language) {
    if (at && WordChar(text[at - 1])) return nullptr;
    for (end = text.size(); end > at; end--) {
        if (end < text.size() && WordChar(text[end])) continue;
        if (const char16_t* found = Translation(text.substr(at, end - at), language)) return found;
    }
    return nullptr;
}

// latin 1 lower case to upper; 0xF7 is the division sign
char16_t Upper(char16_t c) {
    bool lower = (c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
    return lower ? char16_t(c - 0x20) : c;
}

void AppendUtf8(std::string& out, char16_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | c >> 6);
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | c >> 12);
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::string app::Ui(std::string_view text, bool caps) {
    int language = Language();
    if (language < 0) return std::string(text);
    // the panel's thread and the game's
    static std::mutex lock;
    static std::map<std::string, std::string> cache;
    std::lock_guard guard(lock);
    std::string key = std::string(text) + (caps ? "\1" : "\2") + char('0' + language);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = 0;
        const char16_t* found = PhraseAt(text, i, end, language);
        if (!found) {
            out += text[i++];
            continue;
        }
        for (; *found; found++) AppendUtf8(out, caps ? Upper(*found) : *found);
        i = end;
    }
    return cache[key] = out;
}
