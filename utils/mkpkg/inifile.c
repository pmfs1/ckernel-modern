#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "inifile.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

static char *trimstr(char *s, char *end)
{
    char *str;
    char *t;
    int ch;
    int i;

    while (end > s)
    {
        if (*(end - 1) == ' ' || *(end - 1) == '\t')
        {
            end--;
        }
        else
        {
            break;
        }
    }
    if (end == s)
        return NULL;

    t = str = (char *)malloc(end - s + 1);
    while (s < end)
    {
        if (*s == '^')
        {
            s++;
            ch = 0;
            for (i = 0; i < 2; i++)
            {
                if (s == end)
                    break;

                if (*s >= '0' && *s <= '9')
                {
                    ch = (ch << 4) + *s - '0';
                }
                else if (*s >= 'A' && *s <= 'F')
                {
                    ch = (ch << 4) + *s + 10 - 'A';
                }
                else if (*s >= 'a' && *s <= 'f')
                {
                    ch = (ch << 4) + *s + 10 - 'a';
                }
                else
                {
                    break;
                }

                s++;
            }
            *t++ = ch;
        }
        else
        {
            *t++ = *s++;
        }
    }

    *t = 0;
    return str;
}

struct section *find_section(struct section *sect, char *name)
{
    while (sect)
    {
        if (strcmp(sect->name, name) == 0)
            return sect;
        sect = sect->next;
    }

    return NULL;
}

int get_section_size(struct section *sect)
{
    struct property *prop;
    int n;

    if (!sect)
        return 0;
    prop = sect->properties;

    n = 0;
    while (prop)
    {
        n++;
        prop = prop->next;
    }

    return n;
}

char *find_property(struct section *sect, char *name)
{
    struct property *prop;

    if (!sect)
        return NULL;
    prop = sect->properties;

    while (prop)
    {
        if (strcmp(prop->name, name) == 0)
            return prop->value ? prop->value : "";
        prop = prop->next;
    }

    return NULL;
}

char *get_property(struct section *sections, char *sectname, char *propname, char *defval)
{
    struct section *sect;
    char *val;

    sect = find_section(sections, sectname);
    if (!sect)
        return defval;

    val = find_property(sect, propname);
    return val ? val : defval;
}

int get_numeric_property(struct section *sections, char *sectname, char *propname, int defval)
{
    char *val;

    val = get_property(sections, sectname, propname, NULL);
    return val ? atoi(val) : defval;
}

void free_properties(struct section *sect)
{
    struct section *nextsect;
    struct property *prop;
    struct property *nextprop;

    while (sect)
    {
        free(sect->name);

        prop = sect->properties;
        while (prop)
        {
            free(prop->name);
            free(prop->value);

            nextprop = prop->next;
            free(prop);
            prop = nextprop;
        }

        nextsect = sect->next;
        free(sect);
        sect = nextsect;
    }
}

struct section *parse_properties(char *props)
{
    struct section *secthead = NULL;
    struct section *sect = NULL;
    struct property *prop = NULL;
    char *p;
    char *end;
    char *split;

    p = props;
    while (*p)
    {
        // Skip white at start of line
        while (*p == ' ' || *p == '\t')
            p++;

        // Skip comments
        if (*p == '#' || *p == ';')
        {
            while (*p && *p != '\r' && *p != '\n')
                p++;
            if (*p == '\r')
                p++;
            if (*p == '\n')
                p++;
            continue;
        }

        // Skip blank lines
        if (*p == 0 || *p == '\r' || *p == '\n')
        {
            if (*p == '\r')
                p++;
            if (*p == '\n')
                p++;
            continue;
        }

        // Check for section or property
        if (*p == '[')
        {
            struct section *newsect;

            p++;
            end = p;
            while (*end && *end != ']')
                end++;

            newsect = (struct section *)malloc(sizeof(struct section));
            if (!newsect)
                return NULL;

            newsect->name = trimstr(p, end);
            newsect->next = NULL;
            newsect->properties = NULL;
            if (!secthead)
                secthead = newsect;
            if (sect)
                sect->next = newsect;
            sect = newsect;
            prop = NULL;

            p = end;
            if (*p == ']')
                p++;
        }
        else
        {
            struct property *newprop;

            end = p;
            split = NULL;
            while (*end && *end != '\r' && *end != '\n')
            {
                if (!split && (*end == '=' || *end == ':'))
                    split = end;
                end++;
            }

            if (sect)
            {
                newprop = (struct property *)malloc(sizeof(struct property));
                if (!newprop)
                    return NULL;

                if (split)
                {
                    newprop->name = trimstr(p, split);
                    split++;
                    while (*split == ' ' || *split == '\t')
                        split++;
                    newprop->value = trimstr(split, end);
                }
                else
                {
                    newprop->name = trimstr(p, end);
                    newprop->value = NULL;
                }

                newprop->next = NULL;
                if (prop)
                {
                    prop->next = newprop;
                }
                else
                {
                    sect->properties = newprop;
                }

                prop = newprop;
            }

            p = end;
            if (*p == '\r')
                p++;
            if (*p == '\n')
                p++;
        }
    }

    return secthead;
}

struct section *read_properties(char *filename)
{
    int f;
    int size;
    struct stat buffer;
    char *props;
    struct section *sect;

    f = open(filename, O_BINARY);
    if (f < 0)
        return NULL;

    if (fstat(f, &buffer) < 0)
    {
        close(f);
        return NULL;
    }
    size = (int)buffer.st_size;

    props = (char *)malloc(size + 1);
    if (!props)
    {
        close(f);
        return NULL;
    }

    if (read(f, props, size) != size)
    {
        free(props);
        close(f);
        return NULL;
    }

    close(f);

    props[size] = 0;

    sect = parse_properties(props);
    free(props);

    return sect;
}
