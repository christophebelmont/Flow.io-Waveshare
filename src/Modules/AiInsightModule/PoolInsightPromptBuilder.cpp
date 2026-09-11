/**
 * @file PoolInsightPromptBuilder.cpp
 * @brief Formats pool history and daily weather as bounded OpenAI input.
 */

#include "Modules/AiInsightModule/PoolInsightPromptBuilder.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr char kInstructions[] =
    "RÔLE\n"
    "Tu es un expert en analyse du fonctionnement des piscines automatisées.\n\n"
    "OBJECTIF\n"
    "Produire une analyse courte, pertinente et utile de l'état du bassin à partir des données fournies par Flow.io.\n"
    "L'objectif n'est pas de résumer, reformuler ou décrire les mesures reçues.\n"
    "L'objectif est d'identifier les phénomènes susceptibles d'être en cours dans le bassin, d'en expliquer les causes plausibles, les conséquences possibles et les éventuelles incohérences observées.\n"
    "La valeur ajoutée attendue consiste à fournir des explications qu'un utilisateur ne pourrait pas déduire immédiatement en lisant les chiffres affichés.\n\n"
    "PRINCIPES D'ANALYSE\n"
    "Privilégie toujours :\n"
    "- les phénomènes remarquables ;\n"
    "- les valeurs inhabituelles ;\n"
    "- les évolutions significatives ;\n"
    "- les incohérences entre indicateurs ;\n"
    "- les conséquences possibles sur la qualité de l'eau ;\n"
    "- les conséquences possibles sur le confort de baignade ;\n"
    "- les facteurs environnementaux plausibles ;\n"
    "- les limites ou incertitudes des données disponibles.\n"
    "Une mesure stable mais très éloignée des plages habituellement observées reste un élément majeur à signaler.\n"
    "Ne considère jamais qu'une situation est normale uniquement parce qu'elle est stable.\n"
    "Si une mesure paraît exceptionnellement basse, élevée ou peu crédible, explique pourquoi elle mérite une attention particulière.\n\n"
    "ANALYSE DE COHÉRENCE\n"
    "N'analyse jamais les mesures indépendamment les unes des autres.\n"
    "Cherche les cohérences et incohérences possibles entre :\n"
    "- température de l'eau ;\n"
    "- température extérieure ;\n"
    "- rayonnement solaire ;\n"
    "- météo récente ;\n"
    "- météo prévue ;\n"
    "- chauffage ;\n"
    "- filtration ;\n"
    "- pH ;\n"
    "- ORP ;\n"
    "- état de la désinfection ;\n"
    "- remplissage ;\n"
    "- caractéristiques du bassin ;\n"
    "- couverture éventuelle ;\n"
    "- volume du bassin.\n"
    "Lorsqu'un comportement paraît surprenant, difficile à expliquer ou contradictoire, signale-le explicitement.\n"
    "Propose alors une ou plusieurs hypothèses plausibles sans les présenter comme certaines.\n"
    "Lorsque plusieurs explications sont possibles, classe implicitement les hypothèses de la plus plausible à la plus spéculative.\n\n"
    "CONTEXTE GÉOGRAPHIQUE ET SAISONNIER\n"
    "Utilise la date de génération ainsi que la localisation géographique du bassin pour tenir compte de la saison et du contexte climatique général.\n"
    "Lorsque cela est pertinent, tu peux mentionner des phénomènes saisonniers généralement observés à cette période de l'année et compatibles avec les observations :\n"
    "- chaleur estivale ;\n"
    "- refroidissement saisonnier ;\n"
    "- pollens ;\n"
    "- matières végétales ;\n"
    "- insectes ;\n"
    "- poussières atmosphériques ;\n"
    "- charge organique accrue ;\n"
    "- développement biologique favorisé par une eau chaude ;\n"
    "- autres phénomènes environnementaux génériques.\n"
    "Présente toujours ces éléments comme des hypothèses plausibles.\n"
    "N'affirme jamais l'existence d'une source locale spécifique qui n'est pas explicitement connue (champ agricole, forêt, chantier, végétation particulière, etc.).\n\n"
    "RAISONNEMENT ATTENDU\n"
    "Pour chaque phénomène important :\n"
    "- expliquer ce qui est observé ;\n"
    "- expliquer pourquoi cela est notable ;\n"
    "- expliquer les conséquences possibles ;\n"
    "- proposer les explications plausibles ;\n"
    "- préciser les éventuelles incertitudes.\n"
    "Ne te contente jamais de répéter les mesures fournies.\n\n"
    "CONTRAINTES STRICTES\n"
    "- Ne demande jamais d'augmenter ou diminuer le pH, l'ORP, la désinfection, la filtration, le chauffage ou le remplissage.\n"
    "- Ne recommande aucun réglage, dosage, paramétrage ou action corrective.\n"
    "- Ne donne aucun objectif de consigne.\n"
    "- Ne donne aucune durée de fonctionnement cible.\n"
    "- Ne formule aucun conseil de régulation que Flow.io est supposé gérer automatiquement.\n"
    "- N'invente aucune mesure manquante.\n"
    "- Distingue clairement les observations, les hypothèses et les prévisions.\n"
    "- Ne présente jamais une hypothèse comme un fait établi.\n"
    "- Ne présente jamais la météo comme la cause certaine d'un phénomène.\n"
    "- Lorsque certaines données semblent incohérentes, insuffisantes ou peu plausibles, indique explicitement qu'une information manquante ou une mesure non représentative pourrait être en cause.\n\n"
    "SÉMANTIQUE DES DONNÉES\n\n"
    "Les dates et plages horaires sont exprimées dans l'heure locale du bassin. Pour un équipement, une durée active doit toujours être interprétée par rapport à la durée réellement observée. Une période indisponible signifie inconnue et non inactive. Distingue les états réellement observés, les consignes configurées et les comportements seulement déclarés. Les consignes historiques sont un contexte d'analyse et ne doivent jamais être reformulées comme des recommandations.\n\n"
    "FORMAT DE RÉPONSE\n"
    "Réponds en français.\n"
    "Produis uniquement 3 à 4 paragraphes courts.\n"
    "Pas de titre.\n"
    "Pas de liste.\n"
    "Pas de tableau.\n"
    "Pas de Markdown.\n"
    "Chaque paragraphe doit apporter une information nouvelle.\n"
    "Évite de répéter inutilement les valeurs numériques déjà visibles dans les données.\n"
    "Le commentaire doit se lire comme l'analyse d'un expert technique observant le comportement global du bassin et cherchant à expliquer les phénomènes en cours.";

class TextWriter {
public:
    TextWriter(char* output, size_t capacity) : output_(output), capacity_(capacity)
    {
        if (output_ && capacity_ > 0U) output_[0] = '\0';
        else valid_ = false;
    }

    bool append(const char* text)
    {
        if (!valid_ || !text) return false;
        const size_t length = strlen(text);
        if (length >= remaining_()) {
            valid_ = false;
            return false;
        }
        memcpy(output_ + length_, text, length);
        length_ += length;
        output_[length_] = '\0';
        return true;
    }

    bool appendFormat(const char* format, ...)
    {
        if (!valid_ || !format || remaining_() == 0U) return false;
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(output_ + length_, remaining_(), format, args);
        va_end(args);
        if (written < 0 || (size_t)written >= remaining_()) {
            valid_ = false;
            return false;
        }
        length_ += (size_t)written;
        return true;
    }

    bool valid() const { return valid_; }

private:
    size_t remaining_() const
    {
        return output_ && length_ < capacity_ ? capacity_ - length_ : 0U;
    }

    char* output_ = nullptr;
    size_t capacity_ = 0U;
    size_t length_ = 0U;
    bool valid_ = true;
};

const char* weatherStateName_(AiWeatherState state)
{
    switch (state) {
        case AiWeatherState::Idle: return "en attente";
        case AiWeatherState::Queued: return "planifiée";
        case AiWeatherState::Loading: return "chargement";
        case AiWeatherState::Ready: return "disponible";
        case AiWeatherState::Failed: return "en échec";
    }
    return "inconnue";
}

void appendDate_(TextWriter& out, uint32_t localDate)
{
    if (localDate < 10000101U) {
        out.append("date inconnue");
        return;
    }
    out.appendFormat("%04lu-%02lu-%02lu",
                     (unsigned long)(localDate / 10000U),
                     (unsigned long)((localDate / 100U) % 100U),
                     (unsigned long)(localDate % 100U));
}

void appendOptionalValue_(TextWriter& out,
                          const char* label,
                          const WeatherValueSummary& value,
                          const char* unit)
{
    if (value.valid) out.appendFormat("- %s : %.1f %s\n", label, value.value, unit);
    else out.appendFormat("- %s : indisponible\n", label);
}

void appendWeatherDay_(TextWriter& out,
                       const PoolWeatherDaySummary* weather,
                       const char* prefix)
{
    if (!weather || !weather->valid) {
        out.appendFormat("- %s : indisponible\n", prefix);
        return;
    }
    out.appendFormat("- %s : air", prefix);
    if (weather->minimumAirTemperatureC.valid &&
        weather->maximumAirTemperatureC.valid &&
        weather->meanAirTemperatureC.valid) {
        out.appendFormat(" min %.1f, max %.1f, moyenne %.1f °C",
                         weather->minimumAirTemperatureC.value,
                         weather->maximumAirTemperatureC.value,
                         weather->meanAirTemperatureC.value);
    } else {
        out.append(" indisponible");
    }
    if (weather->precipitationMm.valid) {
        out.appendFormat(" ; précipitations %.1f mm", weather->precipitationMm.value);
    }
    if (weather->meanCloudCoverPercent.valid) {
        out.appendFormat(" ; nébulosité moyenne %.0f %%", weather->meanCloudCoverPercent.value);
    }
    if (weather->maximumWindSpeedKmh.valid) {
        out.appendFormat(" ; vent max %.1f km/h", weather->maximumWindSpeedKmh.value);
    }
    if (weather->shortwaveRadiationMjM2.valid) {
        out.appendFormat(" ; rayonnement %.2f MJ/m²", weather->shortwaveRadiationMjM2.value);
    }
    out.append("\n");
}

const PoolWeatherDaySummary* weatherForDate_(const AiWeatherStatus& weather,
                                             uint32_t localDate)
{
    if (!weather.weather.available) return nullptr;
    for (uint8_t i = 0U; i < weather.weather.dailyCount; ++i) {
        const PoolWeatherDaySummary& day = weather.weather.daily[i];
        if (day.valid && day.localDate == localDate) return &day;
    }
    return nullptr;
}

void appendMetric_(TextWriter& out,
                   const char* label,
                   const PoolHistoryMetricSummary& metric,
                   const char* unit,
                   uint8_t decimals)
{
    if (!metric.valid) {
        out.appendFormat("- %s : indisponible\n", label);
        return;
    }
    const char* format = decimals == 2U
        ? "- %s : début %.2f, fin %.2f, moyenne %.2f, min %.2f, max %.2f %s (%lu mesures)\n"
        : "- %s : début %.1f, fin %.1f, moyenne %.1f, min %.1f, max %.1f %s (%lu mesures)\n";
    out.appendFormat(format, label, metric.first, metric.last, metric.average,
                     metric.minimum, metric.maximum, unit,
                     (unsigned long)metric.sampleCount);
}

void appendActivity_(TextWriter& out,
                     const char* label,
                     const PoolHistoryActivitySummary& activity)
{
    static constexpr const char* kNames[POOL_HISTORY_DAY_PERIOD_COUNT] = {
        "nuit 00-06", "matin 06-12", "après-midi 12-18", "soir 18-24"
    };
    if (!activity.valid) {
        out.appendFormat("- %s : indisponible\n", label);
        return;
    }
    out.appendFormat("- %s total : %.2f h (%lu min) sur %.2f h observées\n",
                     label,
                     activity.runningHours,
                     (unsigned long)activity.runningMinutes,
                     (double)activity.observedSec / 3600.0);
    out.appendFormat("- %s par période (actif/observé) :", label);
    for (uint8_t i = 0U; i < POOL_HISTORY_DAY_PERIOD_COUNT; ++i) {
        const PoolHistoryActivityPeriodSummary& period = activity.periods[i];
        out.appendFormat("%s %s ", i == 0U ? "" : " ;", kNames[i]);
        if (period.valid) {
            out.appendFormat("%lu/%lu min",
                             (unsigned long)(period.runningSec / 60U),
                             (unsigned long)(period.observedSec / 60U));
        } else {
            out.append("indisponible");
        }
    }
    out.append("\n");
}

const char* disinfectionName_(PoolDisinfectionMethod method)
{
    switch (method) {
        case PoolDisinfectionMethod::Disabled: return "désactivée";
        case PoolDisinfectionMethod::ChlorineBromine: return "chlore/brome";
        case PoolDisinfectionMethod::SaltElectrolysis: return "électrolyse au sel";
        case PoolDisinfectionMethod::ActiveOxygen: return "oxygène actif";
    }
    return "inconnue";
}

void appendCurrentContext_(TextWriter& out, const PoolHistorySnapshot& history)
{
    out.append("\nCONTEXTE ACTUEL DU BASSIN\n");
    out.append("- date locale de génération : ");
    appendDate_(out, history.today.localDate);
    out.append("\n");
    if (!history.pool.available) {
        out.append("- configuration du bassin : indisponible\n");
    } else {
        if (history.pool.volumeValid) out.appendFormat("- volume : %.1f m³\n", history.pool.volumeM3);
        else out.append("- volume : indisponible\n");
        out.appendFormat("- implantation : %s\n", history.pool.indoor ? "intérieure" : "extérieure");
        out.appendFormat("- couverture automatique : %s\n",
                         history.pool.automaticCoverPresent ? "présente" : "absente");
        out.appendFormat("- politique déclarée de couverture fermée la nuit : %s\n",
                         history.pool.coverClosedAtNight ? "oui" : "non");
        out.append("- position réelle de la couverture : indisponible (aucun capteur)\n");
        out.appendFormat("- désinfection : %s\n",
                         disinfectionName_(history.pool.disinfectionMethod));
    }
    const PoolOperatingConfiguration& config = history.currentOperatingConfiguration;
    if (!config.available) {
        out.append("- configuration de régulation actuelle : indisponible\n");
        return;
    }
    out.appendFormat("- modes automatiques actuels : filtration %s, pH %s, ORP %s, chauffage %s\n",
                     config.filtrationAutoMode ? "actif" : "inactif",
                     config.phAutoMode ? "actif" : "inactif",
                     config.orpAutoMode ? "actif" : "inactif",
                     config.heaterAutoMode ? "actif" : "inactif");
    if (config.phSetpointValid) out.appendFormat("- consigne pH actuelle : %.2f\n", config.phSetpoint);
    if (config.orpSetpointValid) out.appendFormat("- consigne ORP actuelle : %.1f mV\n", config.orpSetpointMv);
    if (config.heaterSetpointValid) out.appendFormat("- consigne chauffage actuelle : %.1f °C\n", config.heaterSetpointC);
}

void appendDay_(TextWriter& out,
                const char* label,
                const PoolHistoryDaySummary& day,
                const AiWeatherStatus& weather)
{
    out.appendFormat("\n%s (", label);
    appendDate_(out, day.localDate);
    out.append(")\n");
    if (!day.valid) {
        out.append("- historique bassin : indisponible\n");
        appendWeatherDay_(out, weatherForDate_(weather, day.localDate), "météo");
        return;
    }
    out.appendFormat("- statut : %s\n", day.complete ? "journée complète" : "journée en cours");
    appendActivity_(out, "filtration", day.filtration);
    appendActivity_(out, "chauffage", day.heating);
    appendMetric_(out, "pH", day.ph, "", 2U);
    appendMetric_(out, "consigne pH observée", day.phSetpoint, "", 2U);
    appendMetric_(out, "ORP", day.orp, "mV", 1U);
    appendMetric_(out, "consigne ORP observée", day.orpSetpoint, "mV", 1U);
    appendMetric_(out, "température de l'eau", day.waterTemperature, "°C", 1U);
    appendMetric_(out, "température de l'eau en journée", day.daytimeWaterTemperature, "°C", 1U);
    appendMetric_(out, "température de l'eau la nuit", day.nighttimeWaterTemperature, "°C", 1U);
    appendMetric_(out, "consigne chauffage observée", day.heaterSetpoint, "°C", 1U);
    if (day.dayToNightTemperatureVariationValid) {
        out.appendFormat("- variation jour vers nuit : %+.2f °C (moyenne nuit moins moyenne jour)\n",
                         day.dayToNightTemperatureVariationC);
    } else {
        out.append("- variation jour vers nuit : indisponible\n");
    }
    appendMetric_(out, "température de l'air mesurée par Flow.io", day.airTemperature, "°C", 1U);
    if (day.refillVolumeValid && day.refillEventsValid) {
        out.appendFormat("- remplissage : %.2f L, %lu événement(s)\n",
                         day.refillVolumeLitres, (unsigned long)day.refillEventCount);
    } else if (day.refillEventsValid) {
        out.appendFormat("- remplissage : volume indisponible, %lu événement(s)\n",
                         (unsigned long)day.refillEventCount);
    } else {
        out.append("- remplissage : indisponible\n");
    }
    appendWeatherDay_(out, weatherForDate_(weather, day.localDate), "météo");
}

void appendPeriodSummary_(TextWriter& out, const PoolHistoryPeriodSummary& period)
{
    out.append("\nSYNTHÈSE DES SEPT DERNIERS JOURS COMPLETS\n");
    out.appendFormat("- historique bassin : %u/%u journées disponibles\n",
                     (unsigned)period.availableDayCount,
                     (unsigned)period.requestedDayCount);
    if (period.averageDailyFiltrationValid) {
        out.appendFormat("- filtration : total %.2f h ; moyenne %.2f h/j sur %u jours observés\n",
                         period.totalFiltrationHours,
                         period.averageDailyFiltrationHours,
                         (unsigned)period.filtrationAvailableDayCount);
    } else {
        out.append("- filtration : indisponible\n");
    }
    if (period.refillVolumeValid && period.refillEventsValid) {
        out.appendFormat("- remplissage : total %.2f L ; moyenne %.2f L/j ; %lu événements\n",
                         period.totalRefillVolumeLitres,
                         period.averageDailyRefillVolumeLitres,
                         (unsigned long)period.totalRefillEventCount);
    } else {
        out.append("- synthèse du remplissage : indisponible ou partielle\n");
    }
}

}  // namespace

const char* PoolInsightPromptBuilder::instructions()
{
    return kInstructions;
}

bool PoolInsightPromptBuilder::build(const PoolHistorySnapshot* history,
                                     const AiWeatherStatus& weather,
                                     char* weatherText,
                                     size_t weatherTextCapacity,
                                     char* prompt,
                                     size_t promptCapacity)
{
    TextWriter weatherOut(weatherText, weatherTextCapacity);
    weatherOut.appendFormat("État de la collecte météo : %s",
                            weatherStateName_(weather.state));
    if (weather.message[0] != '\0') weatherOut.appendFormat(" (%s)", weather.message);
    weatherOut.append("\n");
    if (weather.weather.available) {
        weatherOut.appendFormat("- position GPS : %.6f, %.6f\n",
                                weather.weather.latitude, weather.weather.longitude);
        appendOptionalValue_(weatherOut, "température extérieure actuelle",
                             weather.weather.currentAirTemperatureC, "°C");
        appendOptionalValue_(weatherOut, "couverture nuageuse actuelle",
                             weather.weather.currentCloudCoverPercent, "%");
        appendOptionalValue_(weatherOut, "vent actuel",
                             weather.weather.currentWindSpeedKmh, "km/h");
        for (uint8_t i = 0U; i < weather.weather.dailyCount; ++i) {
            const PoolWeatherDaySummary& day = weather.weather.daily[i];
            if (!day.valid || !day.forecast) continue;
            weatherOut.append("- météo prévue ");
            appendDate_(weatherOut, day.localDate);
            weatherOut.append("\n");
            appendWeatherDay_(weatherOut, &day, "prévision");
        }
    } else {
        weatherOut.append("- données météo : indisponibles\n");
    }
    if (!weatherOut.valid()) return false;

    TextWriter promptOut(prompt, promptCapacity);
    promptOut.append("DONNÉES FOURNIES PAR FLOW.IO\n\n");
    promptOut.append(weatherText);
    if (!history) {
        promptOut.append("\nHistorique piscine : indisponible\n");
        return promptOut.valid();
    }
    appendCurrentContext_(promptOut, *history);
    appendPeriodSummary_(promptOut, history->last7Days);
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        appendDay_(promptOut, "JOURNÉE COMPLÈTE", history->completeDays[i], weather);
    }
    appendDay_(promptOut, "JOURNÉE EN COURS (CONTEXTE UNIQUEMENT)", history->today, weather);
    promptOut.appendFormat("\nInstant de génération UTC : %llu\n",
                           (unsigned long long)history->generatedAtUtc);
    return promptOut.valid();
}
