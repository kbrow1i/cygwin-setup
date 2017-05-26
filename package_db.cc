/*
 * Copyright (c) 2001, 2003 Robert Collins.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by Robert Collins  <rbtcollins@hotmail.com>
 *
 */

/* this is the package database class.
 * It lists all known packages, including custom ones, ones from a mirror and
 * installed ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <algorithm>
#if HAVE_ERRNO_H
#include <errno.h>
#endif

#include "io_stream.h"
#include "compress.h"

#include "filemanip.h"
#include "package_version.h"
#include "package_db.h"
#include "package_meta.h"
#include "Exception.h"
#include "Generic.h"
#include "LogSingleton.h"
#include "resource.h"
#include "libsolv.h"

using namespace std;

packagedb::packagedb ()
{
}

void
packagedb::read ()
{
  if (!installeddbread)
    {
      solver.internalize();

      /* Read in the local installation database. */
      io_stream *db = 0;
      db = io_stream::open ("cygfile:///etc/setup/installed.db", "rt", 0);
      installeddbread = 1;
      if (!db)
	return;
      char line[1000], pkgname[1000];

      if (db->gets (line, 1000))
	{
	  /* Look for header line (absent in version 1) */
	  int instsz;
	  int dbver;
	  sscanf (line, "%s %d", pkgname, &instsz);
	  if (!strcasecmp (pkgname, "INSTALLED.DB"))
	    dbver = instsz;
	  else
	    dbver = 1;
	  delete db;
	  db = 0;

	  Log (LOG_BABBLE) << "INSTALLED.DB version " << dbver << endLog;

	  if (dbver <= 3)
	    {
	      char inst[1000];

	      db =
		io_stream::open ("cygfile:///etc/setup/installed.db", "rt", 0);

	      // skip over already-parsed header line
	      if (dbver >= 2)
		db->gets (line, 1000);

	      while (db->gets (line, 1000))
		{
		  int parseable;
		  int user_picked = 0;
		  pkgname[0] = '\0';
		  inst[0] = '\0';

		  int res = sscanf (line, "%s %s %d", pkgname, inst, &user_picked);

		  if (res < 3 || pkgname[0] == '\0' || inst[0] == '\0')
			continue;

		  fileparse f;
		  parseable = parse_filename (inst, f);
		  if (!parseable)
		    continue;

                  SolverPool::addPackageData data;
                  data.reponame = "_installed";
                  data.version = f.ver;
                  data.type = package_binary;

                  // very limited information is available from installed.db, so
                  // we put our best guesses here...
                  data.vendor = "cygwin";
                  data.requires = NULL;
                  data.obsoletes = NULL;
                  data.sdesc = "";
                  data.ldesc = "";
                  data.stability = TRUST_CURR; // XXX: would be nice to get this correct as it effects upgrade decisions...

                  // supplement this with sdesc and source information from
                  // setup.ini, if possible...
                  packagemeta *pkgm = findBinary(PackageSpecification(pkgname));
                  if (pkgm)
                    {
                      data.sdesc = pkgm->curr.SDesc();
                      data.archive = *pkgm->curr.source();
                      data.spkg = std::string(pkgname) + "-src";
                      data.spkg_id = pkgm->curr.sourcePackage();
                    }

		  packagemeta *pkg = packagedb::addBinary (pkgname, data);

		  pkg->set_installed_version (f.ver);

		  if (dbver == 3)
		    pkg->user_picked = (user_picked & 1);

		}
	      delete db;
	      db = 0;
	    }
	  else
	    {
              fatal(NULL, IDS_INSTALLEDB_VERSION);
	    }

	  installeddbver = dbver;
	}
    }
  solver.internalize();
}

/* Add a package version to the packagedb */
packagemeta *
packagedb::addBinary (const std::string &pkgname,
                      const SolverPool::addPackageData &pkgdata)
{
  /* If pkgname isn't already in packagedb, add a packagemeta */
  packagemeta *pkg = findBinary (PackageSpecification(pkgname));
  if (!pkg)
    {
      pkg = new packagemeta (pkgname);
      packages.insert (packagedb::packagecollection::value_type(pkgname, pkg));
    }

  /* Create the SolvableVersion  */
  SolvableVersion sv = solver.addPackage(pkgname, pkgdata);

  /* Register it in packagemeta */
  pkg->add_version (sv, pkgdata);

  return pkg;
}

packageversion
packagedb::addSource (const std::string &pkgname,
                      const SolverPool::addPackageData &pkgdata)
{
  /* If pkgname isn't already in packagedb, add a packagemeta */
  packagemeta *pkg = findSource (PackageSpecification(pkgname));
  if (!pkg)
    {
      pkg = new packagemeta (pkgname);
      sourcePackages.insert (packagedb::packagecollection::value_type(pkgname, pkg));
    }

  /* Create the SolvableVersion  */
  SolvableVersion sv = solver.addPackage(pkgname, pkgdata);

  /* Register it in packagemeta */
  pkg->add_version (sv, pkgdata);

  return sv;
}

int
packagedb::flush ()
{
  /* naive approach - just dump the lot */
  char const *odbn = "cygfile:///etc/setup/installed.db";
  char const *ndbn = "cygfile:///etc/setup/installed.db.new";

  io_stream::mkpath_p (PATH_TO_FILE, ndbn, 0755);

  io_stream *ndb = io_stream::open (ndbn, "wb", 0644);

  // XXX if this failed, try removing any existing .new database?
  if (!ndb)
    return errno ? errno : 1;

  ndb->write ("INSTALLED.DB 3\n", strlen ("INSTALLED.DB 3\n"));
  for (packagedb::packagecollection::iterator i = packages.begin ();
       i != packages.end (); ++i)
    {
      packagemeta & pkgm = *(i->second);
      if (pkgm.installed)
	{
	  /*
	    In INSTALLED.DB 3, lines are: 'packagename version flags', where
	    version is encoded in a notional filename for backwards
	    compatibility, and the only currently defined flag is user-picked
	    (bit 0).
	  */
	  std::string line;
	  line = pkgm.name + " " +
	    pkgm.name + "-" + std::string(pkgm.installed.Canonical_version()) + ".tar.bz2 " +
	    (pkgm.user_picked ? "1" : "0") + "\n";
	  ndb->write (line.c_str(), line.size());
	}
    }

  delete ndb;

  io_stream::remove (odbn);

  if (io_stream::move (ndbn, odbn))
    return errno ? errno : 1;
  return 0;
}

void
packagedb::upgrade()
{
  if (installeddbver < 3)
    {
      /* Guess which packages were user_picked.  This has to take place after
         setup.ini has been parsed as it needs dependency information. */
      guessUserPicked();
      installeddbver = 3;
    }
}

packagemeta *
packagedb::findBinary (PackageSpecification const &spec) const
{
  packagedb::packagecollection::iterator n = packages.find(spec.packageName());
  if (n != packages.end())
    {
      packagemeta & pkgm = *(n->second);
      for (set<packageversion>::iterator i=pkgm.versions.begin();
	  i != pkgm.versions.end(); ++i)
	if (spec.satisfies (*i))
	  return &pkgm;
    }
  return NULL;
}

packagemeta *
packagedb::findSource (PackageSpecification const &spec) const
{
  packagedb::packagecollection::iterator n = sourcePackages.find(spec.packageName());
  if (n != sourcePackages.end())
    {
      packagemeta & pkgm = *(n->second);
      for (set<packageversion>::iterator i = pkgm.versions.begin();
	   i != pkgm.versions.end(); ++i)
	if (spec.satisfies (*i))
	  return &pkgm;
    }
  return NULL;
}

packageversion
packagedb::findSourceVersion (PackageSpecification const &spec) const
{
  packagedb::packagecollection::iterator n = sourcePackages.find(spec.packageName());
  if (n != sourcePackages.end())
    {
      packagemeta & pkgm = *(n->second);
      for (set<packageversion>::iterator i = pkgm.versions.begin();
           i != pkgm.versions.end(); ++i)
        if (spec.satisfies (*i))
          return *i;
    }
  return packageversion();
}

/* static members */

int packagedb::installeddbread = 0;
int packagedb::installeddbver = 0;
packagedb::packagecollection packagedb::packages;
packagedb::categoriesType packagedb::categories;
packagedb::packagecollection packagedb::sourcePackages;
PackageDBActions packagedb::task = PackageDB_Install;
std::vector <packagemeta *> packagedb::dependencyOrderedPackages;
SolverPool packagedb::solver;
SolverSolution packagedb::solution(packagedb::solver);

#include <stack>

class
ConnectedLoopFinder
{
public:
  ConnectedLoopFinder(void);
  void doIt(void);
private:
  size_t visit (packagemeta *pkg);

  packagedb db;
  size_t visited;

  typedef std::map<packagemeta *, size_t> visitMap;
  visitMap visitOrder;
  std::stack<packagemeta *> nodesInStronglyConnectedComponent;
};

ConnectedLoopFinder::ConnectedLoopFinder() : visited(0)
{
  for (packagedb::packagecollection::iterator i = db.packages.begin ();
       i != db.packages.end (); ++i)
    visitOrder.insert(visitMap::value_type(i->second, 0));
}

void
ConnectedLoopFinder::doIt()
{
  /* XXX this could be done useing a class to hold both the visitedInIteration and the package
   * meta reference. Then we could use a range, not an int loop. 
   */
  /* We have to expect dependency loops.  These loops break the topological
     sorting which would be a result of the below algorithm looking for
     strongly connected components in a directed graph.  Unfortunately it's
     not possible to order a directed graph with loops topologially.
     So we always have to make sure that the really important packages don't
     introduce dependency loops, since we can't do this from within setup. */
  for (packagedb::packagecollection::iterator i = db.packages.begin ();
       i != db.packages.end (); ++i)
    {
      packagemeta &pkg (*(i->second));
      if (pkg.installed && !visitOrder[&pkg])
	visit (&pkg);
    }
  Log (LOG_BABBLE) << "Visited: " << visited << " nodes out of "
                   << db.packages.size() << " while creating dependency order."
                   << endLog;
}

static bool
checkForInstalled (PackageSpecification *spec)
{
  packagedb db;
  packagemeta *required = db.findBinary (*spec);
  if (!required)
    return false;
  if (spec->satisfies (required->installed)
      && required->desired == required->installed )
    /* done, found a satisfactory installed version that will remain
       installed */
    return true;
  return false;
}

size_t
ConnectedLoopFinder::visit(packagemeta *nodeToVisit)
{
  if (!nodeToVisit->installed)
    /* Can't visit this node, and it is not less than any visted node */
    return db.packages.size() + 1;

  if (visitOrder[nodeToVisit])
    return visitOrder[nodeToVisit];

  ++visited;
  visitOrder[nodeToVisit] = visited;

#if DEBUG
  Log (LOG_PLAIN) << "visited '" << nodeToVisit->name << "', assigned id " << visited << endLog;
#endif

  size_t minimumVisitId = visited;
  nodesInStronglyConnectedComponent.push(nodeToVisit);

  /* walk through each node */
  const PackageDepends deps = nodeToVisit->installed.depends();
  PackageDepends::const_iterator dp = deps.begin();
  while (dp != deps.end())
    {
      /* check for an installed match */
      if (checkForInstalled (*dp))
	{
	  /* we found an installed ok package */
	  /* visit it if needed */
	  /* UGLY. Need to refactor. iterators in the outer would help as we could simply
	   * vist the iterator
	   */
	  const packagedb::packagecollection::iterator n = db.packages.find((*dp)->packageName());

	  if (n == db.packages.end())
	     Log (LOG_PLAIN) << "Search for package '" << (*dp)->packageName() << "' failed." << endLog;
	   else
	   {
	       packagemeta *nodeJustVisited = n->second;
	       minimumVisitId = std::min (minimumVisitId, visit (nodeJustVisited));
	   }
	}
	/* not installed or not available we ignore */
      ++dp;
    }

  if (minimumVisitId == visitOrder[nodeToVisit])
  {
    packagemeta *popped;
    do {
      popped = nodesInStronglyConnectedComponent.top();
      nodesInStronglyConnectedComponent.pop();
      db.dependencyOrderedPackages.push_back(popped);
      /* mark as displayed in a connected component */
      visitOrder[popped] = db.packages.size() + 2;
    } while (popped != nodeToVisit);
  }

  return minimumVisitId;
}

PackageDBConnectedIterator
packagedb::connectedBegin()
{
  if (!dependencyOrderedPackages.size())
    {
      ConnectedLoopFinder doMe;
      doMe.doIt();
      std::string s = "Dependency order of packages: ";
      
      for (std::vector<packagemeta *>::iterator i =
           dependencyOrderedPackages.begin();
           i != dependencyOrderedPackages.end(); ++i)
        s = s + (*i)->name + " ";
#if DEBUG
      Log (LOG_BABBLE) << s << endLog;
#endif
    }
  return dependencyOrderedPackages.begin();
}

PackageDBConnectedIterator
packagedb::connectedEnd()
{
  return dependencyOrderedPackages.end();
}

void
packagedb::setExistence ()
{
  /* binary packages */
  /* Remove packages that are in the db, not installed, and have no 
     mirror info and are not cached for both binary and source packages. */
  packagedb::packagecollection::iterator i = packages.begin ();
  while (i != packages.end ())
    {
      packagemeta & pkg = *(i->second);
      if (!pkg.installed && !pkg.accessible() && 
          !pkg.sourceAccessible() )
        {
          packagemeta *pkgm = (*i).second;
          delete pkgm;
          packages.erase (i++);
        }
      else
        ++i;
    }

#if 0
  /* remove any source packages which are not accessible */
  vector <packagemeta *>::iterator i = db.sourcePackages.begin();
  while (i != db.sourcePackages.end())
    {
      packagemeta & pkg = **i;
      if (!packageAccessible (pkg))
    {
   packagemeta *pkgm = *i;
   delete pkgm;
      i = db.sourcePackages.erase (i);
    }
      else
 ++i;
    }
#endif
}

void
packagedb::fillMissingCategory ()
{
  for (packagedb::packagecollection::iterator i = packages.begin(); i != packages.end(); i++)
    {
      if (i->second->hasNoCategories())
        i->second->setDefaultCategories();

      i->second->addToCategoryAll();
    }
}

void
packagedb::defaultTrust (trusts trust)
{
  for (packagedb::packagecollection::iterator i = packages.begin (); i != packages.end (); ++i)
    {
      packagemeta & pkg = *(i->second);
      if (pkg.installed
            || pkg.categories.find ("Base") != pkg.categories.end ()
            || pkg.categories.find ("Orphaned") != pkg.categories.end ())
        {
          pkg.desired = pkg.trustp (true, trust);
          if (pkg.desired)
            pkg.pick (pkg.desired.accessible() && pkg.desired != pkg.installed);
        }
      else
        pkg.desired = packageversion ();
    }

  // side effect, remove categories with no packages.
  for (packagedb::categoriesType::iterator n = packagedb::categories.begin();
       n != packagedb::categories.end(); ++n)
    if (!n->second.size())
      {
        Log (LOG_BABBLE) << "Removing empty category " << n->first << endLog;
        packagedb::categories.erase (n++);
      }
}

void
packagedb::guessUserPicked()
{
  /*
    Assume that any non-base installed package which is a dependency of an
    installed package wasn't user_picked

    i.e. only installed packages which aren't in the base category, and aren't
    a dependency of any installed package are user_picked
  */

  /* First mark all installed non-base packages */
  for (packagedb::packagecollection::iterator i = packages.begin ();
       i != packages.end (); ++i)
    {
      packagemeta & pkgm = *(i->second);

      if (pkgm.categories.find ("Base") != pkgm.categories.end ())
	continue;

      if (pkgm.installed)
	pkgm.user_picked = TRUE;
    }

  /* Then clear the mark for all dependencies of all installed packages */
  for (packagedb::packagecollection::iterator i = packages.begin ();
       i != packages.end (); ++i)
    {
      packagemeta & pkgm = *(i->second);

      if (!pkgm.installed)
	continue;

      /* walk through each node */
      const PackageDepends deps = pkgm.installed.depends();
      std::vector <PackageSpecification *>::const_iterator dp = deps.begin();
      while (dp != deps.end())
	{
	  /* check for an installed match */
          if (checkForInstalled(*dp))
	    {
	      const packagedb::packagecollection::iterator n = packages.find((*dp)->packageName());
	      if (n != packages.end())
		{
		  packagemeta *pkgm2 = n->second;
		  pkgm2->user_picked = FALSE;
		}
	      /* skip to next and clause */
	      ++dp;
	      continue;
	    }
	  ++dp;
	}
    }
}
