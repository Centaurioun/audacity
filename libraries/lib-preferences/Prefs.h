/**********************************************************************

  Audacity: A Digital Audio Editor

  Prefs.h

  Dominic Mazzoni
  Markus Meyer

  Audacity uses wxWidgets' wxFileConfig class to handle preferences.
  In Audacity versions prior to 1.3.1, it used wxConfig, which would
  store the prefs in a platform-dependent way (e.g. in the registry
  on Windows). Now it always stores the settings in a configuration file
  in the Audacity Data Directory.

  Every time we read a preference, we need to specify the default
  value for that preference, to be used if the preference hasn't
  been set before.

  So, to avoid code duplication, we provide functions in this file
  to read and write preferences which have a nonobvious default
  value, so that if we later want to change this value, we only
  have to change it in one place.

  See Prefs.cpp for a (complete?) list of preferences we keep
  track of...

**********************************************************************/
#ifndef __AUDACITY_PREFS__
#define __AUDACITY_PREFS__

#include <functional>
#include <set>

// Increment this every time the prefs need to be reset
// the first part (before the r) indicates the version the reset took place
// the second part (after the r) indicates the number of times the prefs have been reset within the same version
#define AUDACITY_PREFS_VERSION_STRING "1.1.1r1"

#include <functional>

#include "ComponentInterfaceSymbol.h"
#include "wxArrayStringEx.h"
#include "FileConfig.h"

#include <memory>

class wxFileName;

PREFERENCES_API void InitPreferences( std::unique_ptr<FileConfig> uPrefs );
//! Call this to reset preferences to an (almost)-"new" default state
/*!
 There is at least one exception to that: user preferences we want to make
 more "sticky."  Notably, whether automatic update checking is preferred.
 */
PREFERENCES_API void ResetPreferences();
PREFERENCES_API void FinishPreferences();

extern PREFERENCES_API FileConfig *gPrefs;
extern int gMenusDirty;


struct ByColumns_t{};
extern PREFERENCES_API ByColumns_t ByColumns;

struct SettingPath {
   RegistryPath mPath;
   operator const RegistryPath &() const { return mPath; }
};

//! Base class for settings objects.  It holds a configuration key path.
/* The constructors are non-explicit for convenience */
class PREFERENCES_API SettingBase
{
public:
   SettingBase( const char *path ) : mPath{ path } {}
   SettingBase( const wxChar *path ) : mPath{ path } {}
   SettingBase( const wxString &path ) : mPath{ path } {}

   wxConfigBase *GetConfig() const;

   const SettingPath &GetPath() const { return mPath; }

   //! Delete the key if present, and return true iff it was.
   bool Delete();

protected:
   SettingBase( const SettingBase& ) = default;
   const SettingPath mPath;
};

class TransactionalSettingBase : public SettingBase
{
public:
   using SettingBase::SettingBase;

   //! @return true iff successful
   virtual bool Commit() = 0;
   virtual void Rollback() noexcept = 0;
   virtual void Invalidate() = 0;
};

//! Makes temporary changes to preferences, then rolls them back at destruction
/*! Nesting of SettingScope is not supported. No copy or move. */
class PREFERENCES_API SettingScope /* not final */
{
public:
   SettingScope();
   ~SettingScope() noexcept;
   SettingScope(const SettingScope&) = delete;
   SettingScope &operator=(const SettingScope&) = delete;

   /*!
    @return `NotAdded` if there is no pending (and uncommitted) scope;
    `PreviouslyAdded` if there is one, but the given setting object was
    already added since it began;
    else `Added`
    */
   enum AddResult{ NotAdded, Added, PreviouslyAdded };
   static AddResult Add( TransactionalSettingBase& setting );

protected:
   static SettingScope *sCurrent;
   std::set< TransactionalSettingBase * > mPending;
   bool mCommitted = false;
};

//! Extend SettingScope with Commit() which flushes updates in a batch
/*! Construct one; then write to some Setting objects; then Commit() before
 destruction to keep the changes, or else the destructor rolls them back.

 Flushes preferences on successful commit.

 Nesting of SettingTransaction is not supported. No copy or move. */
class PREFERENCES_API SettingTransaction final : public SettingScope
{
public:
   //! @return true if successful
   /*! It is unlikely to return false, but in that case an unflushed, partial
    write of changes to the config file may have happened */
   bool Commit();
};

/*! @brief Class template adds an in-memory cache of a value to
 TransactionalSettingBase and support for SettingTransaction
 */
template< typename T >
class CachingSettingBase : public TransactionalSettingBase
{
public:
   using TransactionalSettingBase::TransactionalSettingBase;
   explicit CachingSettingBase( const SettingBase &path )
      : TransactionalSettingBase{ path.GetPath() } {}

protected:
   CachingSettingBase( const CachingSettingBase & ) = default;
   mutable T mCurrentValue{};
   mutable bool mValid{false};
};

//! Class template adds default value, read, and write methods to
//! CachingSetingBase and generates TransactionalSettingBase virtual functions
template< typename T >
class Setting : public CachingSettingBase< T >
{
public:
   using CachingSettingBase< T >::CachingSettingBase;

   using DefaultValueFunction = std::function< T() >;

   //! Usual overload supplies a default value
   Setting( const SettingBase &path, const T &defaultValue )
      : CachingSettingBase< T >{ path }
      , mDefaultValue{ defaultValue }
   {}

   //! This overload causes recomputation of the default each time it is needed
   Setting( const SettingBase &path, DefaultValueFunction function )
      : CachingSettingBase< T >{ path }
      , mFunction{ function }
   {}
   

   const T& GetDefault() const
   {
      if ( mFunction )
         mDefaultValue = mFunction();
      return mDefaultValue;
   }

   //! overload of Read returning a boolean that is true if the value was previously defined  */
   bool Read( T *pVar ) const
   {
      return ReadWithDefault( pVar, GetDefault() );
   }

   //! overload of ReadWithDefault returning a boolean that is true if the value was previously defined  */
   bool ReadWithDefault( T *pVar, const T& defaultValue ) const
   {
      if ( pVar )
         *pVar = defaultValue;
      if ( pVar && this->mValid ) {
         *pVar = this->mCurrentValue;
         return true;
      }
      const auto config = this->GetConfig();
      if ( pVar && config ) {
         if ((this->mValid = config->Read( this->mPath, &this->mCurrentValue )))
            *pVar = this->mCurrentValue;
         return this->mValid;
      }
      return (this->mValid = false);
   }

   //! overload of Read, always returning a value
   /*! The value is the default stored in this in case the key is known to be absent from the config;
    but it returns type T's default value if there was failure to read the config */
   T Read() const
   {
      return ReadWithDefault( GetDefault() );
   }

   //! new direct use is discouraged but it may be needed in legacy code
   /*! Use the given default in case the preference is not defined, which may not be the
    default-default stored in this object. */
   T ReadWithDefault( const T &defaultValue ) const
   {
      if (this->mValid)
         return this->mCurrentValue;
      const auto config = this->GetConfig();
      if (config) {
         this->mCurrentValue =
            config->ReadObject(this->mPath, defaultValue);
         // If config file contains a value that agrees with the default, we
         // can't detect that, so assume invalidity still
         this->mValid = (this->mCurrentValue != defaultValue);
         return this->mCurrentValue;
      }
      else
         return T{};
   }

   //! Write value to config and return true if successful
   bool Write( const T &value )
   {
      switch ( SettingScope::Add( *this ) ) {
         // Eager writes, but not flushed, when there is no transaction
         default:
         case SettingTransaction::NotAdded: {
            const auto config = this->GetConfig();
            if ( config ) {
               this->mCurrentValue = value;
               return DoWrite();
            }
            return false;
         }

         // Deferred writes, with flush, if there is a commit later
         case SettingTransaction::Added:
            this->mPreviousValue = Read();
            [[fallthrough]];
         case SettingTransaction::PreviouslyAdded:
            this->mCurrentValue = value;
            return true;
      }
   }

   //! Reset to the default value
   bool Reset()
   {
      return Write( GetDefault() );
   }

   bool Commit() override
   {
      return DoWrite();
   }

   void Invalidate() override
   {
      this->mValid = false;
   }

   void Rollback() noexcept override
   {
      this->mCurrentValue = this->mPreviousValue;
   }

protected:
   //! Write cached value to config and return true if successful
   /*! (But the config object is not flushed) */
   bool DoWrite( )
   {
      const auto config = this->GetConfig();
      return this->mValid =
         config ? config->Write( this->mPath, this->mCurrentValue ) : false;
   }

   const DefaultValueFunction mFunction;
   mutable T mDefaultValue{};
   T mPreviousValue{};
};

//! This specialization of Setting for bool adds a Toggle method to negate the saved value
class PREFERENCES_API BoolSetting final : public Setting< bool >
{
public:
   using Setting::Setting;

   //! Write the negation of the previous value, and then return the current value.
   bool Toggle();
};

//! Specialization of Setting for int
class IntSetting final : public Setting< int >
{
public:
   using Setting::Setting;
};

//! Specialization of Setting for double
class DoubleSetting final : public Setting< double >
{
public:
   using Setting::Setting;
};

//! Specialization of Setting for strings
class StringSetting final : public Setting< wxString >
{
public:
   using Setting::Setting;
};

using EnumValueSymbol = ComponentInterfaceSymbol;

/// A table of EnumValueSymbol that you can access by "row" with
/// operator [] but also allowing access to the "columns" of internal or
/// translated strings, and also allowing convenient column-wise construction
class PREFERENCES_API EnumValueSymbols : public std::vector< EnumValueSymbol >
{
public:
   EnumValueSymbols() = default;
   EnumValueSymbols( std::initializer_list<EnumValueSymbol> symbols )
     : vector( symbols )
   {}
   EnumValueSymbols( std::vector< EnumValueSymbol > symbols )
     : vector( symbols )
   {}

   // columnwise constructor; arguments must have same size
   // (Implicit constructor takes initial tag argument to avoid unintended
   // overload resolution to the inherited constructor taking
   // initializer_list, in the case that each column has exactly two strings)
   EnumValueSymbols(
      ByColumns_t,
      const TranslatableStrings &msgids,
      wxArrayStringEx internals
   );

   const TranslatableStrings &GetMsgids() const;
   const wxArrayStringEx &GetInternals() const;

private:
   mutable TranslatableStrings mMsgids;
   mutable wxArrayStringEx mInternals;
};

/// Packages a table of user-visible choices each with an internal code string,
/// a preference key path, and a default choice
class PREFERENCES_API ChoiceSetting
{
public:
   //! Disallow construction from the GetPath() of another SettingBase object;
   //! instead require that object to be passed as reference to the next ctor
   ChoiceSetting(const SettingPath &, EnumValueSymbols, long = -1) = delete;

   //! @pre `defaultSymbol < static_cast<long>(mSymbols.size())`
   ChoiceSetting(TransactionalSettingBase &key, EnumValueSymbols symbols,
      long defaultSymbol = -1)
      : mKey{ key.GetPath() }
      , mSymbols{ move(symbols) }
      , mpOtherSettings{ &key }
      , mDefaultSymbol{ defaultSymbol }
   {
      assert(defaultSymbol < static_cast<long>(mSymbols.size()));
   }

   //! @pre `defaultSymbol < static_cast<long>(symbols.size())`
   ChoiceSetting(const SettingBase &key, EnumValueSymbols symbols,
      long defaultSymbol = -1)
      : mKey{ key.GetPath() }
      , mSymbols{ move(symbols) }
      , mDefaultSymbol{ defaultSymbol }
   {
      assert(defaultSymbol < static_cast<long>(mSymbols.size()));
   }

   const wxString &Key() const { return mKey; }
   const EnumValueSymbol &Default() const;
   const EnumValueSymbols &GetSymbols() const { return mSymbols; }

   wxString Read() const;

   // new direct use is discouraged but it may be needed in legacy code:
   // use a default in case the preference is not defined, which may not be
   // the default-default stored in this object.
   wxString ReadWithDefault( const wxString & ) const;

   bool Write( const wxString &value ); // you flush gPrefs afterward

   //! @pre `defaultSymbol < static_cast<long>(GetSymbols().size())`
   void SetDefault( long value );

protected:
   size_t Find( const wxString &value ) const;
   virtual void Migrate( wxString& );

   const wxString mKey;
   const EnumValueSymbols mSymbols;
   TransactionalSettingBase *const mpOtherSettings{};

   // stores an internal value
   mutable bool mMigrated { false };

   long mDefaultSymbol;
};

/// Extends ChoiceSetting with a corresponding table of integer codes
/// (generally not equal to their table positions),
/// and optionally an old preference key path that stored integer codes, to be
/// migrated into one that stores internal string values instead
class PREFERENCES_API EnumSettingBase : public ChoiceSetting {
public:
   //! @pre `intValues.size() == symbols.size()`
   template<typename Key>
   EnumSettingBase(
      Key &&key, // moved string, or lvalue reference to another Setting
      EnumValueSymbols symbols,
      long defaultSymbol,

      std::vector<int> intValues, // must have same size as symbols
      const wxString &oldKey = {}
   )  : ChoiceSetting{ std::forward<Key>(key), move(symbols), defaultSymbol }
      , mIntValues{ move(intValues) }
      , mOldKey{ oldKey }
   {
      assert (mIntValues.size() == mSymbols.size());
   }

protected:

   // Read and write the encoded values
   int ReadInt() const;

   // new direct use is discouraged but it may be needed in legacy code:
   // use a default in case the preference is not defined, which may not be
   // the default-default stored in this object.
   int ReadIntWithDefault( int defaultValue ) const;

   bool WriteInt( int code ); // you flush gPrefs afterward

   size_t FindInt( int code ) const;
   void Migrate( wxString& ) override;

private:
   std::vector<int> mIntValues;
   const wxString mOldKey;
};

/// Adapts EnumSettingBase to a particular enumeration type
template< typename Enum >
class EnumSetting : public EnumSettingBase
{
public:

   //! @pre `intValues.size() == symbols.size()`
   template<typename Key>
   EnumSetting(
      Key &&key, // moved string, or lvalue reference to another Setting
      EnumValueSymbols symbols,
      long defaultSymbol,

      std::vector< Enum > values, // must have same size as symbols
      const wxString &oldKey = {}
   )  : EnumSettingBase{ std::forward<Key>(key), move(symbols), defaultSymbol,
         { values.begin(), values.end() }, oldKey }
   {}

   // Wrap ReadInt() and ReadIntWithDefault() and WriteInt()
   Enum ReadEnum() const
   { return static_cast<Enum>( ReadInt() ); }

   // new direct use is discouraged but it may be needed in legacy code:
   // use a default in case the preference is not defined, which may not be
   // the default-default stored in this object.
   Enum ReadEnumWithDefault( Enum defaultValue ) const
   {
      auto integer = static_cast<int>(defaultValue);
      return static_cast<Enum>( ReadIntWithDefault( integer ) );
   }

   bool WriteEnum( Enum value )
   { return WriteInt( static_cast<int>( value ) ); }

};

//! A listener notified of changes in preferences
class PREFERENCES_API PrefsListener
{
public:
   //! Call this static function to notify all PrefsListener objects
   /*!
    @param id when positive, passed to UpdateSelectedPrefs() of all listeners,
    meant to indicate that only a certain subset of preferences have changed;
    else their UpdatePrefs() methods are called.  (That is supposed to happen
    when the user OK's changes in the Preferences dialog.)
    Callbacks are delayed, in the main thread, using BasicUI::CallAfter
    */
   static void Broadcast(int id = 0);

   PrefsListener();
   virtual ~PrefsListener();

   // Called when all preferences should be updated.
   // PrefsListener::UpdatePrefs() is defined, and does nothing
   virtual void UpdatePrefs() = 0;

protected:
   // Called when only selected preferences are to be updated.
   // id is some value generated by wxNewId() that identifies the portion
   // of preferences.
   // Default function does nothing.
   virtual void UpdateSelectedPrefs( int id );

private:
   struct Impl;
   std::unique_ptr<Impl> mpImpl;
};

/// Return the config file key associated with a warning dialog identified
/// by internalDialogName.  When the box is checked, the value at the key
/// becomes false.
PREFERENCES_API
wxString WarningDialogKey(const wxString &internalDialogName);

/*!
 Meant to be statically constructed.  A callback to repopulate configuration
 files after a reset.
 */
struct PREFERENCES_API PreferenceInitializer {
   PreferenceInitializer();
   virtual ~PreferenceInitializer();
   virtual void operator () () = 0;

   static void ReinitializeAll();
};

// Special extra-sticky settings
extern PREFERENCES_API BoolSetting DefaultUpdatesCheckingFlag;

#endif
