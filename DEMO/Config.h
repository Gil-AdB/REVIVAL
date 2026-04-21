/*
	header file Config.h
	Revival / FLD
	Version 1.00


*/
#ifndef _FLD_DEMO_CONFIG_H_INCLUDED
#define _FLD_DEMO_CONFIG_H_INCLUDED

#include "Base/FDS_DECS.H"
#include <vector>

char *fld_strdup(const char *s);

class CFGEntry
{
protected:
	char *_id;
public:
	virtual mword write(FILE *F) = 0;
	virtual int32_t toInteger() = 0;
	virtual char *toString() = 0;

	CFGEntry()
	{
		_id = NULL;
	}
	virtual ~CFGEntry()
	{
		delete [] _id;
	}
	inline const char *getID() const
	{
		return _id;
	}
	inline char *copyID()
	{
		return fld_strdup(_id);
	}
	inline void setID(char *id)
	{
		_id = fld_strdup(id);
	}
};

class CFGInteger : public CFGEntry
{
	int32_t _value;
public:
	inline int32_t getValue()
	{
		return _value;
	}
	inline void setValue(int32_t value)
	{
		_value = value;
	}

	inline CFGInteger()
	{
		_value = 0;
	}
	inline CFGInteger(const char *id, int32_t value)
	{
		_id = fld_strdup(id);
		_value = value;
	}
	inline ~CFGInteger()
	{
	}

	mword write(FILE *F);
	int32_t toInteger();
	char *toString();

};


class CFGString : public CFGEntry
{
	char *_value;
public:
	inline char *copyValue()
	{
		return fld_strdup(_value);
	}
	inline const char *getValue()
	{
		return _value;
	}
	inline void setValue(char *value)
	{
		_value = fld_strdup(value);
	}

	inline CFGString()
	{
		_value = NULL;
	}
	inline CFGString(char *id, char *value)
	{
		_id = fld_strdup(id);
		_value = fld_strdup(value);
	}
	inline ~CFGString()
	{		
		delete [] _value;
	}

	mword write(FILE *F);
	int32_t toInteger();
	char *toString();
};


class ConfigurationDB
{
	char *_categoryName;
	std::vector<CFGEntry *> _entries;
	std::vector<ConfigurationDB *> _children;

	mword write(FILE *F, int32_t indent);
	mword read(FILE *F, int32_t indent);
	CFGEntry *readEntry(FILE *F);
public:
	void setCategory(const char *name);
	char *copyCategory();
	const char *getCategory();

	void addEntry(CFGEntry *e);

	CFGEntry *find(const char *id);
	int32_t extractInteger(const char *id);
	char *extractString(const char *id);
	mword fromFile(const char *filename);
	mword toFile(const char *filename);

	ConfigurationDB();
	~ConfigurationDB();
};

#endif //_FLD_DEMO_CONFIG_H_INCLUDED