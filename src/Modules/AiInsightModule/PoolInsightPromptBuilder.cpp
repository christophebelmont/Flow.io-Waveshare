/**
 * @file PoolInsightPromptBuilder.cpp
 * @brief Formats pool history and weather into an inspectable OpenAI prompt.
 */

#include "Modules/AiInsightModule/PoolInsightPromptBuilder.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

class TextWriter {
public:
    TextWriter(char* output, size_t capacity)
        : output_(output), capacity_(capacity)
    {
        if (output_ && capacity_ > 0U) output_[0] = '\0';
        else valid_ = false;
    }

    bool append(const char* text)
    {
        if (!valid_ || !text) return false;
        const size_t textLength = strlen(text);
        if (textLength >= remaining_()) {
            valid_ = false;
            return false;
        }
        memcpy(output_ + length_, text, textLength);
        length_ += textLength;
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
        return (output_ && length_ < capacity_) ? capacity_ - length_ : 0U;
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

void appendOptionalValue_(TextWriter& out,
                          const char* label,
                          const WeatherValueSummary& value,
                          const char* unit)
{
    if (value.valid) out.appendFormat("- %s : %.1f %s\n", label, value.value, unit);
    else out.appendFormat("- %s : indisponible\n", label);
}

void appendRange_(TextWriter& out,
                  const char* label,
                  const WeatherRangeSummary& range,
                  const char* unit)
{
    if (range.valid) {
        out.appendFormat("- %s : %.1f à %.1f %s (%u mesures)\n",
                         label,
                         range.minimum,
                         range.maximum,
                         unit,
                         (unsigned)range.sampleCount);
    } else {
        out.appendFormat("- %s : indisponible\n", label);
    }
}

void appendAggregate_(TextWriter& out,
                      const char* label,
                      const WeatherAggregateSummary& aggregate,
                      const char* unit)
{
    if (aggregate.valid) {
        out.appendFormat("- %s : %.1f %s (%u mesures)\n",
                         label,
                         aggregate.value,
                         unit,
                         (unsigned)aggregate.sampleCount);
    } else {
        out.appendFormat("- %s : indisponible\n", label);
    }
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
        ? "- %s : début %.2f, dernière %.2f, moyenne %.2f, min %.2f, max %.2f %s (%lu mesures)\n"
        : "- %s : début %.1f, dernière %.1f, moyenne %.1f, min %.1f, max %.1f %s (%lu mesures)\n";
    out.appendFormat(format,
                     label,
                     metric.first,
                     metric.last,
                     metric.average,
                     metric.minimum,
                     metric.maximum,
                     unit,
                     (unsigned long)metric.sampleCount);
}

void appendDay_(TextWriter& out,
                const char* label,
                const PoolHistoryDaySummary& day)
{
    out.appendFormat("\n%s (", label);
    appendDate_(out, day.localDate);
    out.append(")\n");
    if (!day.valid) {
        out.append("- historique indisponible\n");
        return;
    }
    out.appendFormat("- statut : %s\n", day.complete ? "journée complète" : "journée en cours");
    if (day.filtrationRuntimeValid) {
        out.appendFormat("- filtration : %.2f h (%lu min) sur %.2f h observées\n",
                         day.filtrationRuntimeHours,
                         (unsigned long)day.filtrationRuntimeMinutes,
                         (double)day.filtrationObservedSec / 3600.0);
    } else {
        out.append("- filtration : indisponible\n");
    }
    appendMetric_(out, "pH", day.ph, "", 2U);
    appendMetric_(out, "ORP", day.orp, "mV", 1U);
    appendMetric_(out, "température de l'eau", day.waterTemperature, "°C", 1U);
    appendMetric_(out, "température de l'eau en journée", day.daytimeWaterTemperature, "°C", 1U);
    appendMetric_(out, "température de l'eau la nuit", day.nighttimeWaterTemperature, "°C", 1U);
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

void appendPoolCharacteristics_(TextWriter& out, const PoolHistorySnapshot& history)
{
    out.append("\nCaractéristiques du bassin\n");
    if (!history.pool.available) {
        out.append("- configuration du bassin indisponible\n");
        return;
    }
    if (history.pool.volumeValid) {
        out.appendFormat("- volume : %.1f m³\n", history.pool.volumeM3);
    } else {
        out.append("- volume : indisponible\n");
    }
    out.appendFormat("- implantation : %s\n", history.pool.indoor ? "intérieure" : "extérieure");
    out.appendFormat("- couverture automatique : %s\n",
                     history.pool.automaticCoverPresent ? "présente" : "absente");
    out.appendFormat("- couverture fermée la nuit : %s\n",
                     history.pool.coverClosedAtNight ? "oui" : "non");
    out.appendFormat("- désinfection active : %s\n",
                     disinfectionName_(history.pool.disinfectionMethod));
    out.appendFormat("- période de jour : %02u:00-%02u:00 ; nuit : %02u:00-%02u:00\n",
                     (unsigned)history.daytimeStartHour,
                     (unsigned)history.daytimeEndHour,
                     (unsigned)history.daytimeEndHour,
                     (unsigned)history.daytimeStartHour);
}

void appendPeriodSummary_(TextWriter& out, const PoolHistoryPeriodSummary& period)
{
    out.append("\nSynthèse des sept derniers jours complets\n");
    out.appendFormat("- couverture : %u/%u journées disponibles\n",
                     (unsigned)period.availableDayCount,
                     (unsigned)period.requestedDayCount);
    if (period.averageDailyFiltrationValid) {
        out.appendFormat("- filtration totale : %.2f h (%lu min) ; moyenne disponible : %.2f h/j (%u jours)\n",
                         period.totalFiltrationHours,
                         (unsigned long)period.totalFiltrationMinutes,
                         period.averageDailyFiltrationHours,
                         (unsigned)period.filtrationAvailableDayCount);
    } else {
        out.append("- filtration : indisponible\n");
    }
    if (period.refillVolumeValid && period.refillEventsValid) {
        out.appendFormat("- remplissage total : %.2f L ; moyenne disponible : %.2f L/j ; événements : %lu\n",
                         period.totalRefillVolumeLitres,
                         period.averageDailyRefillVolumeLitres,
                         (unsigned long)period.totalRefillEventCount);
    } else if (period.refillEventsValid) {
        out.appendFormat("- remplissage : volume indisponible ; événements connus : %lu\n",
                         (unsigned long)period.totalRefillEventCount);
    } else {
        out.append("- remplissage : indisponible\n");
    }
}

}  // namespace

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
    if (weather.message[0] != '\0') {
        weatherOut.appendFormat(" (%s)", weather.message);
    }
    weatherOut.append("\n");

    if (weather.weather.available) {
        weatherOut.appendFormat("Position : %.6f, %.6f\n",
                                weather.weather.latitude,
                                weather.weather.longitude);
        weatherOut.appendFormat("Observation météo UTC : %llu\n",
                                (unsigned long long)weather.weather.observedAtUtc);
        appendOptionalValue_(weatherOut, "température extérieure actuelle", weather.weather.currentAirTemperatureC, "°C");
        appendOptionalValue_(weatherOut, "couverture nuageuse actuelle", weather.weather.currentCloudCoverPercent, "%");
        appendOptionalValue_(weatherOut, "vent actuel", weather.weather.currentWindSpeedKmh, "km/h");
        appendRange_(weatherOut, "température extérieure sur les 24 h passées", weather.weather.previous24hAirTemperatureC, "°C");
        appendAggregate_(weatherOut, "précipitations sur les 24 h passées", weather.weather.previous24hPrecipitationMm, "mm");
        appendRange_(weatherOut, "température extérieure prévue sur les 24 h à venir", weather.weather.forecast24hAirTemperatureC, "°C");
        appendAggregate_(weatherOut, "précipitations prévues sur les 24 h à venir", weather.weather.forecast24hPrecipitationMm, "mm");
        appendAggregate_(weatherOut, "nébulosité moyenne prévue sur les 24 h à venir", weather.weather.forecast24hCloudCoverPercent, "%");
        appendAggregate_(weatherOut, "vent maximal prévu sur les 24 h à venir", weather.weather.forecast24hMaximumWindSpeedKmh, "km/h");
        appendAggregate_(weatherOut, "rayonnement solaire moyen prévu sur les 24 h à venir", weather.weather.forecast24hShortwaveRadiationWm2, "W/m²");
    } else {
        weatherOut.append("Aucune donnée météo n'est encore disponible.\n");
    }
    if (!weatherOut.valid()) return false;

    TextWriter promptOut(prompt, promptCapacity);
    promptOut.append(
        "RÔLE\n"
        "Tu analyses l'état d'une piscine pilotée automatiquement par Flow.io.\n\n"
        "OBJECTIF\n"
        "Explique brièvement l'état du bassin et sa dynamique sur les sept derniers jours complets, "
        "puis relie les évolutions observées à la météo passée et prévue lorsqu'un lien est plausible. "
        "Présente l'ensemble comme un commentaire fluide et conclus, si cela est pertinent, par une "
        "courte observation utile au confort d'usage, sans consigne de régulation.\n\n"
        "CONTRAINTES STRICTES\n"
        "- Ne demande jamais de monter ou baisser le pH, l'ORP, la désinfection, la filtration, le chauffage ou le remplissage.\n"
        "- Ne propose aucun réglage, dosage, durée cible, seuil ni action que Flow.io est censé piloter lui-même.\n"
        "- N'invente aucune mesure manquante et distingue clairement observation, hypothèse et prévision.\n"
        "- Ne présente pas la météo comme la cause certaine d'une variation : indique seulement les liens plausibles.\n"
        "- Si la couverture historique est partielle, signale-le et reste prudent dans la comparaison.\n\n"
        "FORMAT DE RÉPONSE\n"
        "Réponds en français, en texte simple et naturel, avec au maximum trois courts paragraphes continus. "
        "N'ajoute aucun titre, sous-titre, libellé, liste à puces ni mise en forme Markdown.\n\n"
        "DONNÉES FOURNIES PAR FLOW.IO\n"
        "Météo :\n");
    promptOut.append(weatherText);
    if (history) {
        appendPoolCharacteristics_(promptOut, *history);
        appendPeriodSummary_(promptOut, history->last7Days);
        for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
            appendDay_(promptOut, "Journée complète", history->completeDays[i]);
        }
        appendDay_(promptOut, "Journée en cours (contexte uniquement)", history->today);
        promptOut.appendFormat("\nInstant de génération UTC : %llu\n",
                               (unsigned long long)history->generatedAtUtc);
    } else {
        promptOut.append("\nHistorique piscine : indisponible\n");
    }
    return promptOut.valid();
}
