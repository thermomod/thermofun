// ThermoFun includes
#include "Common/Exception.h"
#include "DBClient/formuladata.h"
#include "ThermoEngine.h"
#include "Database.h"
#include "Reaction.h"
#include "Substance.h"
#include "Element.h"
#include "ThermoModelsSubstance.h"
#include "ThermoModelsSolvent.h"
#include "ThermoProperties.h"
#include "ElectroModelsSolvent.h"
#include "ThermoModelsReaction.h"

#include "OptimizationUtils.h"

#include <functional>

#include <boost/algorithm/string/predicate.hpp>

namespace ThermoFun {

const std::map<const std::string, const std::string> implemented_conventions = {
{"Benson-Helgeson",  "aparent-properties"   },
{"Berman-Brown",     "aparent-properties"   },
{"steam-tables",     "water-properties"     }};

///
/// \brief The ThermoPreferences struct holds preferences such as the calculation methods for the current substance
///
struct ThermoPreferences
{
    Substance workSubstance;
    Reaction  workReaction;
    MethodGenEoS_Thrift::type method_genEOS;
    MethodCorrT_Thrift::type  method_T;
    MethodCorrP_Thrift::type  method_P;

    unsigned solventState = 0; // 0: liquid; 1: vapor

    bool isHydrogen     = false;
    bool isH2Ovapor     = false;
    bool isH2OSolvent   = false;
    bool isReacDC       = false;
};

using ThermoPropertiesSubstanceFunction =
    std::function<ThermoPropertiesSubstance(double, double, double&, std::string)>;

using ElectroPropertiesSolventFunction =
    std::function<ElectroPropertiesSolvent(double, double, double&, std::string)>;

using PropertiesSolventFunction =
    std::function<PropertiesSolvent(double, double, double&, std::string)>;

using ThermoPropertiesReactionFunction =
    std::function<ThermoPropertiesReaction(double, double, double&, std::string)>;

struct ThermoEngine::Impl
{
    /// The database instance
    Database database;

    std::string solventSymbol = "H2O@"; // default

    const std::map<const std::string, std::string> conventions = {
    {"aparent-properties", "Benson-Helgeson"     },
    {"water-properties", ""    }};

    ThermoPropertiesSubstanceFunction thermo_properties_substance_fn;

    ElectroPropertiesSolventFunction  electro_properties_solvent_fn;

    PropertiesSolventFunction         properties_solvent_fn;

    ThermoPropertiesReactionFunction  thermo_properties_reaction_fn;

    Impl()
    {}

    Impl(const Database& database)
    : database(database)
    {
        thermo_properties_substance_fn = [=](double T, double P_, double &P, std::string symbol)
        {
            return thermoPropertiesSubstance(T, P, symbol);
        };
        thermo_properties_substance_fn = memoize(thermo_properties_substance_fn);

        electro_properties_solvent_fn = [=](double T, double P_, double &P, std::string symbol)
        {
            return electroPropertiesSolvent(T, P, symbol);
        };
        electro_properties_solvent_fn = memoize(electro_properties_solvent_fn);

        properties_solvent_fn = [=](double T, double P_, double &P, std::string symbol)
        {
            return propertiesSolvent(T, P, symbol);
        };
        properties_solvent_fn = memoize(properties_solvent_fn);

        thermo_properties_reaction_fn = [=](double T, double P_, double &P, std::string symbol)
        {
            return thermoPropertiesReaction(T, P, symbol);
        };
        thermo_properties_reaction_fn = memoize(thermo_properties_reaction_fn);
    }

    auto toSteamTables(ThermoPropertiesSubstance &tps) -> void
    {
        // Auxiliary data from Helgeson and Kirkham (1974), on page 1098
        const auto Str =  15.1320 * cal_to_J; // unit: J/(mol*K)
        const auto Gtr = -56290.0 * cal_to_J; // unit: J/mol
        const auto Htr = -68767.0 * cal_to_J; // unit: J/mol
        const auto Utr = -67887.0 * cal_to_J; // unit: J/mol
        const auto Atr = -55415.0 * cal_to_J; // unit: J/mol

        tps.gibbs_energy    -=Gtr; tps.enthalpy         -=Htr;
        tps.entropy         -=Str; tps.helmholtz_energy -=Atr;
        tps.internal_energy -=Utr;
    }

    auto toBermanBrown(ThermoPropertiesSubstance &tps, const Substance &subst) -> void
    {
        const auto Tr = subst.referenceT();
        FormulaProperites prop = ThermoFun::ChemicalFormula::calcThermo(subst.formula());
        const auto entropyElements = prop.elemental_entropy;
        tps.gibbs_energy -= (Tr*entropyElements);
        tps.enthalpy     -= (Tr*entropyElements);
    }

    auto getThermoPreferences(std::string substance) -> ThermoPreferences
    {
        ThermoPreferences preferences;
        preferences.workSubstance    = database.getSubstance(substance);
        preferences.method_genEOS    = preferences.workSubstance.methodGenEOS();
        preferences.method_T         = preferences.workSubstance.method_T();
        preferences.method_P         = preferences.workSubstance.method_P();

        // check for H+
        if (preferences.workSubstance.name() == "H+")
            preferences.isHydrogen = true;
        else
            preferences.isHydrogen = false;

        // check for H2O vapor
        if (preferences.method_genEOS == MethodGenEoS_Thrift::type::CTPM_HKF && preferences.method_P == MethodCorrP_Thrift::type::CPM_GAS)
            preferences.isH2Ovapor = true;
        else
            preferences.isH2Ovapor = false;

        // check for solvent
        if (preferences.workSubstance.substanceClass() == SubstanceClass::type::AQSOLVENT /*&& !isH2Ovapor*/)
            preferences.isH2OSolvent = true;
        else
            preferences.isH2OSolvent = false;

        // set solvent state
        if (preferences.workSubstance.aggregateState() == AggregateState::type::GAS)
            preferences.solventState = 1;  // vapor
        else
            preferences.solventState = 0;  // liquid

        // check if the substance is reaction dependent
        if (preferences.workSubstance.thermoCalculationType() == SubstanceThermoCalculationType::type::REACDC)
            preferences.isReacDC = true;
        else
            preferences.isReacDC = false;

        // make check if substance is aq solute and needs a solvent
        return preferences;
    }

    auto thermoPropertiesSubstance(double T, double &P, std::string substance) -> ThermoPropertiesSubstance
    {
        ThermoPreferences         pref = getThermoPreferences(substance);
        ThermoPropertiesSubstance tps;

        if (pref.isHydrogen)
        {
            return tps;
        }

        if (!pref.isReacDC)
        {
            if (!pref.isH2OSolvent && !pref.isH2Ovapor)
            {
                // metohd EOS
                switch( pref.method_genEOS )
                {
                case MethodGenEoS_Thrift::type::CTPM_CPT:
                {
                    tps = EmpiricalCpIntegration(pref.workSubstance).thermoProperties(T, P);
                    break;
                }
                case MethodGenEoS_Thrift::type::CTPM_HKF:
                {
                    tps = SoluteHKFgems(pref.workSubstance).thermoProperties(T, P, properties_solvent_fn(T, P, P, solventSymbol), electro_properties_solvent_fn(T, P, P, solventSymbol));
                    break;
                }
                case MethodGenEoS_Thrift::type::CTPM_HKFR:
                {
                    tps = SoluteHKFreaktoro(pref.workSubstance).thermoProperties(T, P, properties_solvent_fn(T, P, P, solventSymbol), electro_properties_solvent_fn(T, P, P, solventSymbol));
                    break;
                }
                    //                default:
                    //                // Exception
                    //                errorMethodNotFound("substance", pref.workSubstance.symbol(), __LINE__);
                }

                // method T
                switch ( pref.method_T )
                {
                case MethodCorrT_Thrift::type::CTM_CHP:
                {
                    tps = HPLandau(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                    //                default:
                    //               // Exception
                    //                errorMethodNotFound("substance", pref.workSubstance.symbol(), __LINE__);
                }

                // method P
                switch ( pref.method_P )
                {
                case MethodCorrP_Thrift::type::CPM_AKI:
                {
                    double Pr = database.getSubstance(solventSymbol).referenceP();
                    double Tr = database.getSubstance(solventSymbol).referenceT();
                    tps = SoluteAkinfievDiamondEOS(pref.workSubstance).thermoProperties(T, P, tps, thermo_properties_substance_fn(T, P, P,  solventSymbol),
                                                                                        WaterIdealGasWoolley(database.getSubstance(solventSymbol)).thermoProperties(T, P),
                                                                                        properties_solvent_fn(T, P, P, solventSymbol),
                                                                                        thermo_properties_substance_fn(Tr, Pr, Pr,  solventSymbol),
                                                                                        WaterIdealGasWoolley(database.getSubstance(solventSymbol)).thermoProperties(Tr, Pr),
                                                                                        properties_solvent_fn(Tr, Pr, Pr, solventSymbol));
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_CEH:
                {
                    tps = MinMurnaghanEOSHP98(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_VBE:
                {
                    tps = MinBerman88(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_VBM:
                {
                    tps = MinBMGottschalk(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_CORK:
                {
                    tps = GasCORK(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_PRSV:
                {
                    tps = GasPRSV(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_EMP:
                {
                    tps = GasCGF(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_SRK:
                {
                    tps = GasSRK(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_PR78:
                {
                    tps = GasPR78(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_STP:
                {
                    tps = GasSTP(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_CON: // Molar volume assumed independent of T and P
                {
                    tps = ConMolVol(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                case MethodCorrP_Thrift::type::CPM_OFF:
                {
                    tps = IdealGasLawVol(pref.workSubstance).thermoProperties(T, P, tps);
                    break;
                }
                    //                default:
                    //                // Exception
                    //                errorMethodNotFound("substance", pref.workSubstance.symbol(), __LINE__);
                }
            }

            if (pref.isH2OSolvent || pref.isH2Ovapor)
            {
                switch(pref.method_T)
                {
                case MethodCorrT_Thrift::type::CTM_WAT:
                {
                    tps = WaterHGK(pref.workSubstance).thermoPropertiesSubstance(T, P, pref.solventState);
                    break;
                }
                case MethodCorrT_Thrift::type::CTM_WAR:
                {
                    tps = WaterHGKreaktoro(pref.workSubstance).thermoPropertiesSubstance(T, P, pref.solventState);
                    break;
                }
                case MethodCorrT_Thrift::type::CTM_WWP:
                {
                    tps = WaterWP95reaktoro(pref.workSubstance).thermoPropertiesSubstance(T, P, pref.solventState);
                    break;
                }
                case MethodCorrT_Thrift::type::CTM_WZD:
                {
                    tps = WaterZhangDuan2005(pref.workSubstance).thermoPropertiesSubstance(T, P, pref.solventState);
                    break;
                }
                default:
                    switch( pref.method_genEOS )
                    {
                    case MethodGenEoS_Thrift::type::CTPM_CPT:
                    {
                        tps = EmpiricalCpIntegration(pref.workSubstance).thermoProperties(T, P);
                        break;
                    }
                        //                    default:
                    }
                    //                default:
                    //                // Exception
                    //                errorMethodNotFound("substance", pref.workSubstance.symbol(), __LINE__);
                }

            }

            /// Convetion convert
            if (pref.isH2OSolvent) {
                if (boost::iequals(conventions.at("water-properties"),"steam-tables")) {
                    toSteamTables(tps);
                }
            } else {
                if (boost::iequals(conventions.at("aparent-properties"),"Berman-Brown")) {
                    toBermanBrown(tps, pref.workSubstance);
                }
            }
        } else // substance properties calculated using the properties of a reaction
        {
            tps = reacDCthermoProperties(T, P, pref.workSubstance);
        }
        return tps;
    }

    auto electroPropertiesSolvent(double T, double &P, std::string solvent) -> ElectroPropertiesSolvent
    {
        ThermoPreferences        pref = getThermoPreferences(solvent);
        PropertiesSolvent        ps = properties_solvent_fn(T, P, P, solvent); /*propertiesSolvent(T, P, solvent);*/
        ElectroPropertiesSolvent eps;

        if (pref.isH2OSolvent)
        {
            switch(pref.method_genEOS)
            {
            case MethodGenEoS_Thrift::type::CTPM_WJNR:
            {
                eps = WaterJNreaktoro(pref.workSubstance).electroPropertiesSolvent(T, P, ps);
                break;
            }
            case MethodGenEoS_Thrift::type::CTPM_WJNG:
            {
                eps = WaterJNgems(pref.workSubstance).electroPropertiesSolvent(T, P/*, ps*/ );
                break;
            }
            case MethodGenEoS_Thrift::type::CTPM_WSV14:
            {
                eps = WaterElectroSverjensky2014(pref.workSubstance).electroPropertiesSolvent(T, P/*, ps*/);
                break;
            }
            case MethodGenEoS_Thrift::type::CTPM_WF97:
            {
                eps = WaterElectroFernandez1997(pref.workSubstance).electroPropertiesSolvent(T, P/*, ps*/);
                break;
            }
    //            default:
    //            // Exception
    //            errorMethodNotFound("solvent", pref.workSubstance.symbol(), __LINE__);
            }
        }
        return eps;
    }

    auto propertiesSolvent(double T, double &P, std::string solvent) -> PropertiesSolvent
    {
        ThermoPreferences pref = getThermoPreferences(solvent);
        PropertiesSolvent ps;

        if (pref.isH2OSolvent)
        {
            switch(pref.method_T)
            {
            case MethodCorrT_Thrift::type::CTM_WAT:
            {
                ps = WaterHGK(pref.workSubstance).propertiesSolvent(T, P, pref.solventState);
                break;
            }
            case MethodCorrT_Thrift::type::CTM_WAR:
            {
                ps = WaterHGKreaktoro(pref.workSubstance).propertiesSolvent(T, P, pref.solventState);
                break;
            }
            case MethodCorrT_Thrift::type::CTM_WWP:
            {
                ps = WaterWP95reaktoro(pref.workSubstance).propertiesSolvent(T, P, pref.solventState);
                break;
            }
            case MethodCorrT_Thrift::type::CTM_WZD:
            {
                ps = WaterZhangDuan2005(pref.workSubstance).propertiesSolvent(T, P, pref.solventState);
                break;
            }
                //            default:
                //            // Exception
                //            errorMethodNotFound("solvent", pref.workSubstance.symbol(), __LINE__);
            }
        }
        return ps;
    }

    auto thermoPropertiesReaction (double T, double &P, std::string reaction) -> ThermoPropertiesReaction
    {
        ThermoPropertiesReaction tpr;
        auto reac    = database.getReaction(reaction);
        auto methodT = reac.method_T();
        auto methodP = reac.method_P();

        switch (methodT)
        {
        case MethodCorrT_Thrift::type::CTM_LGX:
        case MethodCorrT_Thrift::type::CTM_LGK:
        case MethodCorrT_Thrift::type::CTM_EK0:
        case MethodCorrT_Thrift::type::CTM_EK1:
        case MethodCorrT_Thrift::type::CTM_EK3:
        case MethodCorrT_Thrift::type::CTM_EK2:
        {
            tpr = Reaction_LogK_fT(reac).thermoProperties(T, P, methodT);
            break;
        }
        case MethodCorrT_Thrift::type::CTM_DKR: // Marshall-Franck density model
        {
            tpr = ReactionFrantzMarshall(reac).thermoProperties(T, P, properties_solvent_fn(T, P, P, solventSymbol));
            break;
        }
        case MethodCorrT_Thrift::type::CTM_MRB: // Calling modified Ryzhenko-Bryzgalin model TW KD 08.2007
        {
            return tpr = ReactionRyzhenkoBryzgalin(reac).thermoProperties(T, P, properties_solvent_fn(T, P, P, solventSymbol)); // NOT TESTED!!!
            break;
        }
        case MethodCorrT_Thrift::type::CTM_IKZ:
        {
            // calc_r_interp( q, p, CE, CV );
            break;
        }
    //    default:
    //        // Exception
    //        errorMethodNotFound("reaction", reac.name(), __LINE__);
        }


        switch (methodP)
        {

        case MethodCorrP_Thrift::type::CPM_VKE:
        case MethodCorrP_Thrift::type::CPM_VBE:

        {
            tpr = Reaction_Vol_fT(reac).thermoProperties(T, P);
            break;
        }
        case MethodCorrP_Thrift::type::CPM_NUL:
        case MethodCorrP_Thrift::type::CPM_CON:
        {
            //    if( CV == CPM_CON || CV == CPM_NUL )
            //    {
            //            P_Pst = aW.twp->P - Pst;
            //            VP = Vst * P_Pst;
            //				// VT = Vst * T_Tst;
            //            aW.twp->dG += VP;
            //            aW.twp->dH += VP;
            //    }
            //    // Calculating pressure correction to logK
            //    aW.twp->lgK -= aW.twp->dV * (aW.twp->P - aW.twp->Pst) / aW.twp->RT / lg_to_ln;
            break;
        }
    //    default:
    //    // Exception
    //    errorMethodNotFound("reaction", reac.name(), __LINE__);
        }

    // make a new method P ???
    // line 1571 m_reac2.cpp
    //    if(( rc[q].pstate[0] == CP_GAS || rc[q].pstate[0] == CP_GASI ) && aW.twp->P > 0.0 )
    //    { // molar volume from the ideal gas law
    //        aW.twp->dV = T / aW.twp->P * R_CONSTANT;
    //    }
        //     // Calculating pressure correction to logK
    //    aW.twp->lgK -= aW.twp->dV * (aW.twp->P - aW.twp->Pst) / aW.twp->RT / lg_to_ln;

        return tpr;
    }

    auto reacDCthermoProperties(double T, double &P, Substance subst) -> ThermoPropertiesSubstance
    {
        ThermoPropertiesSubstance       tps, reacTps;
        ThermoPropertiesReaction        tpr;
        string                          reactionSymbol = subst.reactionSymbol();
        Reaction                        reaction;
        std::map<std::string, double>   reactants;

        if (!reactionSymbol.empty())
        {
            reaction = database.getReaction(reactionSymbol);

            tpr = thermo_properties_reaction_fn(T, P, P, reactionSymbol); /*thermoPropertiesReaction(T, P, reactionSymbol);*/

            tps.enthalpy         = tpr.reaction_enthalpy;
            tps.entropy          = tpr.reaction_entropy;
            tps.gibbs_energy     = tpr.reaction_gibbs_energy;
            tps.heat_capacity_cp = tpr.reaction_heat_capacity_cp;
            tps.heat_capacity_cv = tpr.reaction_heat_capacity_cv;
            tps.helmholtz_energy = tpr.reaction_helmholtz_energy;
            tps.internal_energy  = tpr.reaction_internal_energy;
            tps.volume           = tpr.reaction_volume;

            reactants = reaction.reactants();

            for(auto reactant : reactants)
            {
                if (reactant.first != subst.symbol())
                {
                    reacTps =  thermo_properties_substance_fn(T,P,P, reactant.first);  /* thermoPropertiesSubstance(T, P, reactant.first);*/
                    tps.enthalpy         -= reacTps.enthalpy*reactant.second;
                    tps.entropy          -= reacTps.entropy*reactant.second;
                    tps.gibbs_energy     -= reacTps.gibbs_energy*reactant.second;
                    tps.heat_capacity_cp -= reacTps.heat_capacity_cp*reactant.second;
                    tps.heat_capacity_cv -= reacTps.heat_capacity_cv*reactant.second;
                    tps.helmholtz_energy -= reacTps.helmholtz_energy*reactant.second;
                    tps.internal_energy  -= reacTps.internal_energy*reactant.second;
                    tps.volume           -= reacTps.volume*reactant.second;
                }
            }

            tps.enthalpy         = tps.enthalpy/reactants[subst.symbol()];
            tps.entropy          = tps.entropy/reactants[subst.symbol()];
            tps.gibbs_energy     = tps.gibbs_energy/reactants[subst.symbol()];
            tps.heat_capacity_cp = tps.heat_capacity_cp/reactants[subst.symbol()];
            tps.heat_capacity_cv = tps.heat_capacity_cv/reactants[subst.symbol()];
            tps.helmholtz_energy = tps.helmholtz_energy/reactants[subst.symbol()];
            tps.internal_energy  = tps.internal_energy/reactants[subst.symbol()];
            tps.volume           = tps.volume/reactants[subst.symbol()];
        } else
        {
            errorReactionNotDefined(subst.symbol(), __LINE__, __FILE__);
        }

        return tps;
    }
};

//ThermoEngine::ThermoEngine()
//: pimpl(new Impl())
//{}

ThermoEngine::ThermoEngine(const std::string dataset)
: pimpl(new Impl(*(new const Database(dataset))))
{}

ThermoEngine::ThermoEngine(const Database& database)
: pimpl(new Impl(database))
{}

auto ThermoEngine::thermoPropertiesSubstance(double T, double &P, std::string substance) -> ThermoPropertiesSubstance
{
    return pimpl->thermo_properties_substance_fn(T, P, P, substance);
}

auto ThermoEngine::electroPropertiesSolvent(double T, double &P, std::string solvent) -> ElectroPropertiesSolvent
{
    return pimpl->electro_properties_solvent_fn(T, P, P, solvent);
}

auto ThermoEngine::propertiesSolvent(double T, double &P, std::string solvent) -> PropertiesSolvent
{
    return pimpl->properties_solvent_fn(T, P, P, solvent);
}

// Reaction
auto ThermoEngine::thermoPropertiesReaction (double T, double &P, std::string reaction) -> ThermoPropertiesReaction
{
    return pimpl->thermo_properties_reaction_fn(T, P, P, reaction);
}

auto ThermoEngine::thermoPropertiesReactionFromReactants (double T, double &P, std::string symbol) -> ThermoPropertiesReaction
{
    ThermoPropertiesReaction  tpr;
    tpr.reaction_heat_capacity_cp   = 0.0;
    tpr.reaction_gibbs_energy       = 0.0;
    tpr.reaction_enthalpy           = 0.0;
    tpr.reaction_entropy            = 0.0;
    tpr.reaction_volume             = 0.0;
    tpr.ln_equilibrium_constant     = 0.0;
    tpr.log_equilibrium_constant    = 0.0;

    Reaction reaction   = pimpl->database.getReaction(symbol);
    string message      = "Calculated from the reaction components: " + reaction.symbol() + "; ";

    for (auto &reactant : reaction.reactants())
    {
        auto coeff      = reactant.second;
        auto substance  = reactant.first;
        auto tps        = pimpl->thermo_properties_substance_fn(T, P, P, substance); /*thermoPropertiesSubstance(T, P, substance);*/

        tpr.reaction_heat_capacity_cp   += tps.heat_capacity_cp*coeff;
        tpr.reaction_gibbs_energy       += tps.gibbs_energy*coeff;
        tpr.reaction_enthalpy           += tps.enthalpy*coeff;
        tpr.reaction_entropy            += tps.entropy*coeff;
        tpr.reaction_volume             += tps.volume*coeff;
        tpr.ln_equilibrium_constant     = tpr.reaction_gibbs_energy / -(R_CONSTANT*(T));
        tpr.log_equilibrium_constant    = tpr.ln_equilibrium_constant * ln_to_lg;

        setMessage(tps.heat_capacity_cp.sta.first, "Cp of component " + substance, message+tps.heat_capacity_cp.sta.second, tpr.reaction_heat_capacity_cp.sta.second);
        setMessage(tps.gibbs_energy.sta.first,     "G0 of component " + substance, message+tps.gibbs_energy.sta.second,     tpr.reaction_gibbs_energy.sta.second);
        setMessage(tps.enthalpy.sta.first,         "H0 of component " + substance, message+tps.enthalpy.sta.second,         tpr.reaction_enthalpy.sta.second);
        setMessage(tps.entropy.sta.first,          "S0 of component " + substance, message+tps.entropy.sta.second,          tpr.reaction_entropy.sta.second);
        setMessage(tps.volume.sta.first,           "V0 of component " + substance, message+tps.volume.sta.second,           tpr.reaction_volume.sta.second);
        setMessage(tps.gibbs_energy.sta.first,     "G0 of component " + substance, message+tps.gibbs_energy.sta.second,     tpr.log_equilibrium_constant.sta.second);
        setMessage(tps.gibbs_energy.sta.first,     "G0 of component " + substance, message+tps.gibbs_energy.sta.second,     tpr.ln_equilibrium_constant.sta.second);
    }
    return tpr;
}

auto ThermoEngine::setSolventSymbol(const std::string solvent_symbol) -> void
{
    pimpl->solventSymbol = solvent_symbol;
}

auto ThermoEngine::solventSymbol( ) const -> std::string
{
    return pimpl->solventSymbol;
}

auto ThermoEngine::database() -> const Database
{
    return pimpl->database;
}

auto ThermoEngine::parseSubstanceFormula(std::string formula) -> std::map<Element, double>
{
    return pimpl->database.parseSubstanceFormula(formula);
}

} // namespace ThermoFun