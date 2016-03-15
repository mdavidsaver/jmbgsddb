
#include <limits>

#include <boost/numeric/ublas/lu.hpp>

#include "scsi/constants.h"
#include "scsi/moment2.h"

#include "scsi/h5loader.h"

namespace {
// http://www.crystalclearsoftware.com/cgi-bin/boost_wiki/wiki.pl?LU_Matrix_Inversion
void inverse(Moment2ElementBase::value_t& out, const Moment2ElementBase::value_t& in)
{
    using boost::numeric::ublas::permutation_matrix;
    using boost::numeric::ublas::lu_factorize;
    using boost::numeric::ublas::lu_substitute;
    using boost::numeric::ublas::identity_matrix;

    Moment2ElementBase::value_t scratch(in); // copy
    permutation_matrix<size_t> pm(scratch.size1());
    if(lu_factorize(scratch, pm)!=0)
        throw std::runtime_error("Failed to invert matrix");
    out.assign(identity_matrix<double>(scratch.size1()));
    //out = identity_matrix<double>(scratch.size1());
    lu_substitute(scratch, pm, out);
}

} // namespace

Moment2State::Moment2State(const Config& c)
    :StateBase(c)
    ,pos(c.get<double>("L", 0.0))
    ,Ekinetic(c.get<double>("IonEk", 0.0))
    ,sync_phase(c.get<double>("IonFy", 0.0)) // TODO: sync_phase from pos?
    ,moment0(maxsize, 0.0)
    ,state(boost::numeric::ublas::identity_matrix<double>(maxsize))
{
    try{
        const std::vector<double>& I = c.get<std::vector<double> >("moment0");
        if(I.size()>moment0.size())
            throw std::invalid_argument("Initial moment0 size too big");
        std::copy(I.begin(), I.end(), moment0.begin());
    }catch(key_error&){
        // default to zeros
    }catch(boost::bad_any_cast&){
        throw std::invalid_argument("'initial' has wrong type (must be vector)");
    }

    try{
        const std::vector<double>& I = c.get<std::vector<double> >("initial");
        if(I.size()>state.data().size())
            throw std::invalid_argument("Initial state size too big");
        std::copy(I.begin(), I.end(), state.data().begin());
    }catch(key_error&){
        // default to identity
    }catch(boost::bad_any_cast&){
        throw std::invalid_argument("'initial' has wrong type (must be vector)");
    }

    double Erest = c.get<double>("Es", 1.0);
    gamma = (Ekinetic+Erest)/Erest;
    beta = sqrt(1+1.0/sqr(gamma));
}

Moment2State::~Moment2State() {}

Moment2State::Moment2State(const Moment2State& o, clone_tag t)
    :StateBase(o, t)
    ,pos(o.pos)
    ,Ekinetic(o.Ekinetic)
    ,moment0(o.moment0)
    ,state(o.state)
{}

void Moment2State::assign(const StateBase& other)
{
    const Moment2State *O = dynamic_cast<const Moment2State*>(&other);
    if(!O)
        throw std::invalid_argument("Can't assign State: incompatible types");
    pos = O->pos;
    Ekinetic = O->Ekinetic;
    sync_phase = O->sync_phase;
    gamma = O->gamma;
    beta = O->beta;
    moment0 = O->moment0;
    state = O->state;
    StateBase::assign(other);
}

void Moment2State::show(std::ostream& strm) const
{
    strm<<"State: energy="<<Ekinetic<<" moment0="<<moment0<<" state="<<state<<"\n";
}

bool Moment2State::getArray(unsigned idx, ArrayInfo& Info) {
    if(idx==0) {
        Info.name = "state";
        Info.ptr = &state(0,0);
        Info.type = ArrayInfo::Double;
        Info.ndim = 2;
        Info.dim[0] = state.size1();
        Info.dim[1] = state.size2();
        return true;
    } else if(idx==1) {
        Info.name = "moment0";
        Info.ptr = &moment0(0);
        Info.type = ArrayInfo::Double;
        Info.ndim = 1;
        Info.dim[0] = moment0.size();
        return true;
    } else if(idx==2) {
        Info.name = "pos";
        Info.ptr = &pos;
        Info.type = ArrayInfo::Double;
        Info.ndim = 0;
        return true;
    } else if(idx==3) {
        Info.name = "Ekinetic";
        Info.ptr = &Ekinetic;
        Info.type = ArrayInfo::Double;
        Info.ndim = 0;
        return true;
    } else if(idx==4) {
        Info.name = "sync_phase";
        Info.ptr = &sync_phase;
        Info.type = ArrayInfo::Double;
        Info.ndim = 0;
        return true;
    } else if(idx==5) {
        Info.name = "gamma";
        Info.ptr = &gamma;
        Info.type = ArrayInfo::Double;
        Info.ndim = 0;
        return true;
    } else if(idx==6) {
        Info.name = "beta";
        Info.ptr = &beta;
        Info.type = ArrayInfo::Double;
        Info.ndim = 0;
        return true;
    }
    return StateBase::getArray(idx-7, Info);
}

Moment2ElementBase::Moment2ElementBase(const Config& c)
    :ElementVoid(c)
    ,transfer(state_t::maxsize, state_t::maxsize)
    ,transfer_raw(boost::numeric::ublas::identity_matrix<double>(state_t::maxsize))
    ,misalign(boost::numeric::ublas::identity_matrix<double>(state_t::maxsize))
    ,misalign_inv(state_t::maxsize, state_t::maxsize)
    ,scratch(state_t::maxsize, state_t::maxsize)
{
    length = c.get<double>("L", 0.0);
    FSampLength = C0/c.get<double>("Frf")*MtoMM;
    phase_factor = length*2*M_PI/FSampLength;
    Erest = c.get<double>("IonEs");

    inverse(misalign_inv, misalign);

    // spoil to force recalculation of energy dependent terms
    last_Kenergy_in = last_Kenergy_out = std::numeric_limits<double>::quiet_NaN();
}

Moment2ElementBase::~Moment2ElementBase() {}

void Moment2ElementBase::assign(const ElementVoid *other)
{
    const Moment2ElementBase *O = static_cast<const Moment2ElementBase*>(other);
    length = O->length;
    FSampLength = O->FSampLength;
    phase_factor = O->phase_factor;
    Erest = O->Erest;
    transfer = O->transfer;
    transfer_raw = O->transfer_raw;
    misalign = O->misalign;
    ElementVoid::assign(other);

    // spoil to force recalculation of energy dependent terms
    last_Kenergy_in = last_Kenergy_out = std::numeric_limits<double>::quiet_NaN();
}

void Moment2ElementBase::show(std::ostream& strm) const
{
    using namespace boost::numeric::ublas;
    ElementVoid::show(strm);
    strm<<"Length "<<length<<"\n"
          "FSampLength "<<FSampLength<<"\n"
          "phase_factor "<<phase_factor<<"\n"
          "Erest "<<Erest<<"\n"
          "Transfer: "<<transfer<<"\n"
          "Transfer Raw: "<<transfer_raw<<"\n"
          "Mis-align: "<<misalign<<"\n";
}

void Moment2ElementBase::advance(StateBase& s)
{
    state_t& ST = static_cast<state_t&>(s);
    using namespace boost::numeric::ublas;

    if(ST.Ekinetic!=last_Kenergy_in) {
        // need to re-calculate energy dependent terms
        // for a passive element (no energy change)

        transfer_raw(state_t::PS_S, state_t::PS_PS) = -2e0*M_PI/(FSampLength*Erest*cube(ST.beta*ST.gamma))*length;

        noalias(scratch) = prod(misalign, transfer_raw);
        noalias(transfer) = prod(scratch, misalign_inv);

        last_Kenergy_in = last_Kenergy_out = ST.Ekinetic; // no energy gain
    }

    ST.pos += length;
    ST.Ekinetic = last_Kenergy_out;
    ST.sync_phase += phase_factor/ST.beta;

    ST.moment0 = prod(transfer, ST.moment0);

    noalias(scratch) = prod(transfer, ST.state);
    noalias(ST.state) = prod(scratch, trans(transfer));
}

namespace {

void Get2by2Matrix(const double L, const double K, const unsigned ind, typename Moment2ElementBase::value_t &M)
{
    // Transport matrix for one plane for a Quadrupole.
    double sqrtK,
           psi,
           cs,
           sn;

    if (K > 0e0) {
        // Focusing.
        sqrtK = sqrt(K);
        psi = sqrtK*L;
        cs = ::cos(psi);
        sn = ::sin(psi);

        M(ind, ind) = M(ind+1, ind+1) = cs;
        if (sqrtK != 0e0)
            M(ind, ind+1) = sn/sqrtK;
        else
            M(ind, ind+1) = L;
        if (sqrtK != 0e0)
            M(ind+1, ind) = -sqrtK*sn;
        else
            M(ind+1, ind) = 0e0;
    } else {
        // Defocusing.
        sqrtK = sqrt(-K);
        psi = sqrtK*L;
        cs = ::cosh(psi);
        sn = ::sinh(psi);

        M(ind, ind) = M(ind+1, ind+1) = cs;
        if (sqrtK != 0e0)
            M(ind, ind+1) = sn/sqrtK;
        else
            M(ind, ind+1) = L;
        if (sqrtK != 0e0)
            M(ind+1, ind) = sqrtK*sn;
        else
            M(ind+1, ind) = 0e0;
    }
}

struct ElementSource : public Moment2ElementBase
{
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementSource(const Config& c)
        :base_t(c), istate(c)
    {}

    virtual void advance(StateBase& s)
    {
        state_t& ST = static_cast<state_t&>(s);
        // Replace state with our initial values
        ST.assign(istate);
    }

    virtual void show(std::ostream& strm) const
    {
        ElementVoid::show(strm);
        strm<<"Initial: "<<istate.state<<"\n";
    }

    state_t istate;
    // note that 'transfer' is not used by this element type

    virtual ~ElementSource() {}

    virtual const char* type_name() const {return "source";}
};

struct ElementMark : public Moment2ElementBase
{
    // Transport (identity) matrix for a Marker.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementMark(const Config& c) :base_t(c){
        length = phase_factor = 0.0;
    }
    virtual ~ElementMark() {}
    virtual const char* type_name() const {return "marker";}
};

struct ElementDrift : public Moment2ElementBase
{
    // Transport matrix for a Drift.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementDrift(const Config& c)
        :base_t(c)
    {
        double L = length*MtoMM; // Convert from [m] to [mm].

        this->transfer_raw(state_t::PS_X, state_t::PS_PX) = L;
        this->transfer_raw(state_t::PS_Y, state_t::PS_PY) = L;
    }
    virtual ~ElementDrift() {}

    virtual const char* type_name() const {return "drift";}
};

struct ElementSBend : public Moment2ElementBase
{
    // Transport matrix for a Gradient Sector Bend (cylindrical coordinates).
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementSBend(const Config& c)
        :base_t(c)
    {
        double L   = c.get<double>("L")*MtoMM,
               phi = c.get<double>("phi"),               // [rad].
               rho = L/phi,
               K   = c.get<double>("K", 0e0)/sqr(MtoMM), // [1/m^2].
               Kx  = K + 1e0/sqr(rho),
               Ky  = -K;

        // Horizontal plane.
        Get2by2Matrix(L, Kx, (unsigned)state_t::PS_X, this->transfer_raw);
        // Vertical plane.
        Get2by2Matrix(L, Ky, (unsigned)state_t::PS_Y, this->transfer_raw);
        // Longitudinal plane.
//        this->transfer_raw(state_t::PS_S,  state_t::PS_S) = L;
    }
    virtual ~ElementSBend() {}

    virtual const char* type_name() const {return "sbend";}
};

struct ElementQuad : public Moment2ElementBase
{
    // Transport matrix for a Quadrupole; K = B2/Brho.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementQuad(const Config& c)
        :base_t(c)
    {
        double L = c.get<double>("L")*MtoMM,
               //B2 = c.get<double>("B2"),
               K = c.get<double>("K", 0e0)/sqr(MtoMM);

        // Horizontal plane.
        Get2by2Matrix(L,  K, (unsigned)state_t::PS_X, this->transfer_raw);
        // Vertical plane.
        Get2by2Matrix(L, -K, (unsigned)state_t::PS_Y, this->transfer_raw);
        // Longitudinal plane.
        // For total path length.
//        this->transfer_raw(state_t::PS_S, state_t::PS_S) = L;
    }
    virtual ~ElementQuad() {}

    virtual const char* type_name() const {return "quadrupole";}
};

struct ElementSolenoid : public Moment2ElementBase
{
    // Transport (identity) matrix for a Solenoid; K = B0/(2 Brho).
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementSolenoid(const Config& c)
        :base_t(c)
    {
        double L = c.get<double>("L")*MtoMM,      // Convert from [m] to [mm].
               K = c.get<double>("K", 0e0)/MtoMM, // Convert from [m] to [mm].
               C = ::cos(K*L),
               S = ::sin(K*L);

        this->transfer_raw(state_t::PS_X, state_t::PS_X)
                = this->transfer_raw(state_t::PS_PX, state_t::PS_PX)
                = this->transfer_raw(state_t::PS_Y, state_t::PS_Y)
                = this->transfer_raw(state_t::PS_PY, state_t::PS_PY)
                = sqr(C);

        if (K != 0e0)
            this->transfer_raw(state_t::PS_X, state_t::PS_PX) = S*C/K;
        else
            this->transfer_raw(state_t::PS_X, state_t::PS_PX) = L;
        this->transfer_raw(state_t::PS_X, state_t::PS_Y) = S*C;
        if (K != 0e0)
            this->transfer_raw(state_t::PS_X, state_t::PS_PY) = sqr(S)/K;
        else
            this->transfer_raw(state_t::PS_X, state_t::PS_PY) = 0e0;

        this->transfer_raw(state_t::PS_PX, state_t::PS_X) = -K*S*C;
        this->transfer_raw(state_t::PS_PX, state_t::PS_Y) = -K*sqr(S);
        this->transfer_raw(state_t::PS_PX, state_t::PS_PY) = S*C;

        this->transfer_raw(state_t::PS_Y, state_t::PS_X) = -S*C;
        if (K != 0e0)
            this->transfer_raw(state_t::PS_Y, state_t::PS_PX) = -sqr(S)/K;
        else
            this->transfer_raw(state_t::PS_Y, state_t::PS_PX) = 0e0;
        if (K != 0e0)
            this->transfer_raw(state_t::PS_Y, state_t::PS_PY) = S*C/K;
        else
            this->transfer_raw(state_t::PS_Y, state_t::PS_PY) = L;

        this->transfer_raw(state_t::PS_PY, state_t::PS_X) = K*sqr(S);
        this->transfer_raw(state_t::PS_PY, state_t::PS_PX) = -S*C;
        this->transfer_raw(state_t::PS_PY, state_t::PS_Y) = -K*S*C;

        // Longitudinal plane.
        // For total path length.
//        this->transfer_raw(state_t::PS_S, state_t::PS_S) = L;
    }
    virtual ~ElementSolenoid() {}

    virtual const char* type_name() const {return "solenoid";}
};

struct ElementRFCavity : public Moment2ElementBase
{
    // Transport matrix for an RF Cavity.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementRFCavity(const Config& c)
        :base_t(c)
    {
        std::string cav_type = c.get<std::string>("cavtype");
        double L             = c.get<double>("L")*MtoMM;         // Convert from [m] to [mm].

        this->transfer_raw(state_t::PS_X, state_t::PS_PX) = L;
        this->transfer_raw(state_t::PS_Y, state_t::PS_PY) = L;
        // For total path length.
//        this->transfer(state_t::PS_S, state_t::PS_S)  = L;
    }
    virtual ~ElementRFCavity() {}

    virtual void advance(StateBase& s)
    {
        state_t& ST = static_cast<state_t&>(s);
        using namespace boost::numeric::ublas;

        if(ST.Ekinetic!=last_Kenergy_in) {
            // need to re-calculate energy dependent terms
            // for a passive element (no energy change)

            transfer_raw(state_t::PS_S, state_t::PS_PS) = -2e0*M_PI/(FSampLength*Erest*cube(ST.beta*ST.gamma))*length;

            transfer = transfer_raw; //TODO misalign*transfer_raw*inverse(misalign)

            last_Kenergy_in = ST.Ekinetic;
            last_Kenergy_out = ST.Ekinetic + 1;
        }

        ST.pos += length;
        ST.Ekinetic = last_Kenergy_out;
        ST.sync_phase += phase_factor/ST.beta;

        ST.moment0 = prod(transfer, ST.moment0);

        noalias(scratch) = prod(transfer, ST.state);
        noalias(ST.state) = prod(scratch, trans(transfer));
    }

    virtual const char* type_name() const {return "rfcavity";}
};

struct ElementStripper : public Moment2ElementBase
{
    // Transport (identity) matrix for a Charge Stripper.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementStripper(const Config& c)
        :base_t(c)
    {
        // Identity matrix.
    }
    virtual ~ElementStripper() {}

    virtual const char* type_name() const {return "stripper";}
};

struct ElementEDipole : public Moment2ElementBase
{
    // Transport matrix for an Electric Dipole.
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementEDipole(const Config& c)
        :base_t(c)
    {
        //double L = c.get<double>("L")*MtoMM;

    }
    virtual ~ElementEDipole() {}

    virtual const char* type_name() const {return "edipole";}
};

struct ElementGeneric : public Moment2ElementBase
{
    typedef Moment2ElementBase base_t;
    typedef typename base_t::state_t state_t;
    ElementGeneric(const Config& c)
        :base_t(c)
    {
        std::vector<double> I = c.get<std::vector<double> >("transfer");
        if(I.size()>this->transfer_raw.data().size())
            throw std::invalid_argument("Initial transfer size too big");
        std::copy(I.begin(), I.end(), this->transfer_raw.data().begin());
    }
    virtual ~ElementGeneric() {}

    virtual const char* type_name() const {return "generic";}
};

} // namespace

void registerMoment2()
{
    Machine::registerState<Moment2State>("MomentMatrix2");

    Machine::registerElement<ElementSource                 >("MomentMatrix2",   "source");

    Machine::registerElement<ElementMark                   >("MomentMatrix2",   "marker");

    Machine::registerElement<ElementDrift                  >("MomentMatrix2",   "drift");

    Machine::registerElement<ElementSBend                  >("MomentMatrix2",   "sbend");

    Machine::registerElement<ElementQuad                   >("MomentMatrix2",   "quadrupole");

    Machine::registerElement<ElementSolenoid               >("MomentMatrix2",   "solenoid");

    Machine::registerElement<ElementRFCavity               >("MomentMatrix2",   "rfcavity");

    Machine::registerElement<ElementStripper               >("MomentMatrix2",   "stripper");

    Machine::registerElement<ElementEDipole                >("MomentMatrix2",   "edipole");

    Machine::registerElement<ElementGeneric                >("MomentMatrix2",   "generic");
}