#include "Config.h"

char *fld_strdup(const char *s)
{
	if (s == NULL)
		return NULL;
	int32_t l = strlen(s)+1;
	char *d = new char [l];
	memcpy(d, s, l);
	return d;
}

mword CFGInteger::write(FILE *F)
{
	// identifier for CFG Integers.
	int type = 0;

	fprintf(F, "%d %s %d\n", type, _id, _value);
	return 0;
}

int32_t CFGInteger::toInteger()
{
	return _value;
}

char *CFGInteger::toString()
{
	char buffer[128];
	sprintf(buffer, "%d", _value);
	return fld_strdup(buffer);
}

mword CFGString::write(FILE *F)
{
	// identifier for CFG Strings
	int type = 1;

	fprintf(F, "%d %s %s\n", type, _id, _value);
	return 0;
}

int32_t CFGString::toInteger()
{
	return int32_t(atoi(_value));
}

char *CFGString::toString()
{
	return fld_strdup(_value);
}

ConfigurationDB::ConfigurationDB()
{
	_categoryName = NULL;
}

ConfigurationDB::~ConfigurationDB()
{
	for(int32_t i=0, n = _entries.size(); i<n; ++i)
	{
		delete _entries[i];
	}
	delete [] _categoryName;
}

const char *ConfigurationDB::getCategory()
{
	return _categoryName;
}

char *ConfigurationDB::copyCategory()
{
	return fld_strdup(_categoryName);
}

void ConfigurationDB::setCategory(const char *name)
{
	_categoryName = fld_strdup(name);
}

void ConfigurationDB::addEntry(CFGEntry *e)
{
	_entries.push_back(e);
}


mword ConfigurationDB::toFile(const char *filename)
{	
	FILE *F = fopen(filename, "wt");
	mword rv = write(F, 0);
	fclose(F);
	return rv;
}

mword ConfigurationDB::fromFile(const char *filename)
{
	FILE *F = fopen(filename, "rt");
	mword rv = read(F, 0);
	fclose(F);
	return rv;
}

mword ConfigurationDB::write(FILE *F, int32_t indent)
{
	int32_t i;
	for(i=0; i<indent; ++i)
		fprintf(F, "\t");
	// Hierarchal DB unsupported	
	int32_t n = _entries.size(), c = _children.size();
	fprintf(F, "Category %s (%d entries, %d subcategories)\n", _categoryName, n, c);
	for(i=0; i<n; ++i)
	{
		for(int32_t j=0; j<=indent; ++j)
			fprintf(F, "\t");
		if (_entries[i]->write(F))
			return 1;
	}
	return 0;
}
mword ConfigurationDB::read(FILE *F, int32_t indent)
{
	// Hierarchal DB unsupported
	char buffer[128];
	int32_t n, c;
	fscanf(F, "Category %s (%d entries, %d subcategories)", buffer, &n, &c);
	setCategory(buffer);
	_entries.resize(n);
	_children.resize(c);
	for(int32_t i=0; i<n; ++i)
	{
		if ((_entries[i] = readEntry(F)) == NULL)
			return 1; // report failure.
	}
	return 0;
}

CFGEntry *ConfigurationDB::readEntry(FILE *F)
{
	// read id & type information. NOTE: buffer overflowable.
	char buffer[128];
	int32_t type = -1;
	fscanf(F, "%d%s", &type, buffer);
	switch (type)
	{
	case 0: 
		{
			int32_t value;
			fscanf(F, "%d", &value);
			return new CFGInteger(buffer, value);
		}
	case 1:
		{
			char value[128];
			fscanf(F, "%127s", &value);
			return new CFGString(buffer, value);
		}
		break;
	default:
		return NULL;
	}
}

CFGEntry *ConfigurationDB::find(const char *id)
{
	// Hierarchal DB unsupported
	for(int i=0, n=_entries.size(); i<n; ++i)
	{
		if (!strcmp(id, _entries[i]->getID()))
		{
			return _entries[i];
		}
	}
	return NULL;
}

int32_t ConfigurationDB::extractInteger(const char *id)
{
	CFGEntry *e = find(id);
	if (e == NULL)
		return 0;
	return e->toInteger();
}

char *ConfigurationDB::extractString(const char *id)
{
	CFGEntry *e = find(id);
	if (e == NULL)
		return NULL;
	return e->toString();
}