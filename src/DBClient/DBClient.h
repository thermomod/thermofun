#ifndef DBCLIENT_H
#define DBCLIENT_H

#include <string>
#include <QFileInfo>
#include <QSettings>

// bonio includes
#include "bsonio/thrift_schema.h"
#include "bsonio/dbgraph.h"

namespace ThermoFun {

struct Database;
struct Substance;
struct Reaction;

struct DBSettings
{
    QSettings *QtSettings;

    QString schemaDir;
    QString localDBDir;
    QString localDBName;
    QFileInfo file;
    QString collName;

    bool useLocalDB;

};

class DBClient
{
    DBSettings settings;

    ThriftSchema schema;

    vector<string> _shemaNames;

    // work params
    bson curRecord;

    // keys list data
    boost::shared_ptr<bsonio::TDBGraph> dbgraph;

    //    // Connect to DataBase
    unique_ptr<bsonio::TDBGraph> reactionVertex;
    unique_ptr<bsonio::TDBGraph> substanceVertex;
    unique_ptr<bsonio::TDBGraph> takesEdge;
    unique_ptr<bsonio::TDBGraph> definesEdge;

    void readSchemaDir( const QString& dirPath );
    void getDataFromPreferencesFile( );
    void resetDBClinet(string curSchemaName, string query);
    bsonio::TDBGraph *newDBClinet(string schemaName, string query);

    void setReactantsFollowingIncomingTakesEdges(std::string _id, Reaction &reaction);

    std::string getDefinedSubstanceSymbol(std::string _id);

public:
    explicit DBClient(std::string settingsFile);
    ~DBClient();

    auto getDatabase(uint sourceTDB) -> Database;


};


}

#endif // DBCLIENT_H
