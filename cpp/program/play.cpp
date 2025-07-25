#include "../program/play.h"

#include "../core/global.h"
#include "../core/fileutils.h"
#include "../core/timer.h"
#include "../program/playutils.h"
#include "../program/setup.h"
#include "../search/asyncbot.h"
#include "../search/searchnode.h"
#include "../dataio/files.h"

#include "../core/test.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------

InitialPosition::InitialPosition()
  :board(),hist(),pla(C_EMPTY)
{}
InitialPosition::InitialPosition(const Board& b, const BoardHistory& h, Player p, bool plainFork, bool sekiFork, bool hintFork, double tw)
  :board(b),hist(h),pla(p),isPlainFork(plainFork),isSekiFork(sekiFork),isHintFork(hintFork),trainingWeight(tw)
{}
InitialPosition::~InitialPosition()
{}


ForkData::~ForkData() {
  for(int i = 0; i<forks.size(); i++)
    delete forks[i];
  forks.clear();
  for(int i = 0; i<sekiForks.size(); i++)
    delete sekiForks[i];
  sekiForks.clear();
}

void ForkData::add(const InitialPosition* pos) {
  std::lock_guard<std::mutex> lock(mutex);
  forks.push_back(pos);
}
const InitialPosition* ForkData::get(Rand& rand) {
  std::lock_guard<std::mutex> lock(mutex);
  if(forks.size() <= 0)
    return NULL;
  testAssert(forks.size() < 0x1FFFffff);
  uint32_t r = rand.nextUInt((uint32_t)forks.size());
  size_t last = forks.size()-1;
  const InitialPosition* pos = forks[r];
  forks[r] = forks[last];
  forks.resize(forks.size()-1);
  return pos;
}

void ForkData::addSeki(const InitialPosition* pos, Rand& rand) {
  std::unique_lock<std::mutex> lock(mutex);
  if(sekiForks.size() >= 1000) {
    testAssert(sekiForks.size() < 0x1FFFffff);
    uint32_t r = rand.nextUInt((uint32_t)sekiForks.size());
    const InitialPosition* oldPos = sekiForks[r];
    sekiForks[r] = pos;
    lock.unlock();
    delete oldPos;
  }
  else {
    sekiForks.push_back(pos);
  }
}
const InitialPosition* ForkData::getSeki(Rand& rand) {
  std::lock_guard<std::mutex> lock(mutex);
  if(sekiForks.size() <= 0)
    return NULL;
  testAssert(sekiForks.size() < 0x1FFFffff);
  uint32_t r = rand.nextUInt((uint32_t)sekiForks.size());
  size_t last = sekiForks.size()-1;
  const InitialPosition* pos = sekiForks[r];
  sekiForks[r] = sekiForks[last];
  sekiForks.resize(sekiForks.size()-1);
  return pos;
}

//------------------------------------------------------------------------------------------------

GameInitializer::GameInitializer(ConfigParser& cfg, Logger& logger)
  :createGameMutex(),rand()
{
  initShared(cfg,logger);
}

GameInitializer::GameInitializer(ConfigParser& cfg, Logger& logger, const string& randSeed)
  :createGameMutex(),rand(randSeed)
{
  initShared(cfg,logger);
}

void GameInitializer::initShared(ConfigParser& cfg, Logger& logger) {

  allowedKoRuleStrs = cfg.getStrings("koRules", Rules::koRuleStrings());
  allowedScoringRuleStrs = cfg.getStrings("scoringRules", Rules::scoringRuleStrings());
  allowedTaxRuleStrs = cfg.getStrings("taxRules", Rules::taxRuleStrings());
  allowedMultiStoneSuicideLegals = cfg.getBools("multiStoneSuicideLegals");
  allowedButtons = cfg.getBools("hasButtons");

  for(size_t i = 0; i < allowedKoRuleStrs.size(); i++)
    allowedKoRules.push_back(Rules::parseKoRule(allowedKoRuleStrs[i]));
  for(size_t i = 0; i < allowedScoringRuleStrs.size(); i++)
    allowedScoringRules.push_back(Rules::parseScoringRule(allowedScoringRuleStrs[i]));
  for(size_t i = 0; i < allowedTaxRuleStrs.size(); i++)
    allowedTaxRules.push_back(Rules::parseTaxRule(allowedTaxRuleStrs[i]));

  if(allowedKoRules.size() <= 0)
    throw IOError("koRules must have at least one value in " + cfg.getFileName());
  if(allowedScoringRules.size() <= 0)
    throw IOError("scoringRules must have at least one value in " + cfg.getFileName());
  if(allowedTaxRules.size() <= 0)
    throw IOError("taxRules must have at least one value in " + cfg.getFileName());
  if(allowedMultiStoneSuicideLegals.size() <= 0)
    throw IOError("multiStoneSuicideLegals must have at least one value in " + cfg.getFileName());
  if(allowedButtons.size() <= 0)
    throw IOError("hasButtons must have at least one value in " + cfg.getFileName());

  {
    bool hasAreaScoring = false;
    for(int i = 0; i<allowedScoringRules.size(); i++)
      if(allowedScoringRules[i] == Rules::SCORING_AREA)
        hasAreaScoring = true;
    bool hasTrueButton = false;
    for(int i = 0; i<allowedButtons.size(); i++)
      if(allowedButtons[i])
        hasTrueButton = true;
    if(!hasAreaScoring && hasTrueButton)
      throw IOError("If scoringRules does not include AREA, hasButtons must be false in " + cfg.getFileName());
  }

  if(cfg.contains("bSizes") == cfg.contains("bSizesXY"))
    throw IOError("Must specify exactly one of bSizes or bSizesXY");

  if(cfg.contains("bSizes")) {
    std::vector<int> allowedBEdges = cfg.getInts("bSizes", 2, Board::MAX_LEN);
    std::vector<double> allowedBEdgeRelProbs = cfg.getDoubles("bSizeRelProbs",0.0,1e100);
    double relProbSum = 0.0;
    for(const double p : allowedBEdgeRelProbs)
      relProbSum += p;
    if(relProbSum <= 1e-100)
      throw IOError("bSizeRelProbs must sum to a positive value");
    double allowRectangleProb = cfg.contains("allowRectangleProb") ? cfg.getDouble("allowRectangleProb",0.0,1.0) : 0.0;

    if(allowedBEdges.size() <= 0)
      throw IOError("bSizes must have at least one value in " + cfg.getFileName());
    if(allowedBEdges.size() != allowedBEdgeRelProbs.size())
      throw IOError("bSizes and bSizeRelProbs must have same number of values in " + cfg.getFileName());

    allowedBSizes.clear();
    allowedBSizeRelProbs.clear();
    for(int i = 0; i<(int)allowedBEdges.size(); i++) {
      for(int j = 0; j<(int)allowedBEdges.size(); j++) {
        int x = allowedBEdges[i];
        int y = allowedBEdges[j];
        if(x == y) {
          allowedBSizes.push_back(std::make_pair(x,y));
          allowedBSizeRelProbs.push_back(
            (1.0 - allowRectangleProb) * allowedBEdgeRelProbs[i] / relProbSum +
            allowRectangleProb * allowedBEdgeRelProbs[i] * allowedBEdgeRelProbs[j] / relProbSum / relProbSum
          );
        }
        else {
          if(allowRectangleProb > 0.0) {
            allowedBSizes.push_back(std::make_pair(x,y));
            allowedBSizeRelProbs.push_back(
              allowRectangleProb * allowedBEdgeRelProbs[i] * allowedBEdgeRelProbs[j] / relProbSum / relProbSum
            );
          }
        }
      }
    }
  }
  else if(cfg.contains("bSizesXY")) {
    if(cfg.contains("allowRectangleProb"))
      throw IOError("Cannot specify allowRectangleProb when specifying bSizesXY, please adjust the relative frequency of rectangles yourself");
    allowedBSizes = cfg.getNonNegativeIntDashedPairs("bSizesXY", 2, Board::MAX_LEN);
    allowedBSizeRelProbs = cfg.getDoubles("bSizeRelProbs",0.0,1e100);

    double relProbSum = 0.0;
    for(const double p : allowedBSizeRelProbs)
      relProbSum += p;
    if(relProbSum <= 1e-100)
      throw IOError("bSizeRelProbs must sum to a positive value");
  }

  if(!cfg.contains("komiMean") && !(cfg.contains("komiAuto") && cfg.getBool("komiAuto")))
    throw IOError("Must specify either komiMean=<komi value> or komiAuto=True in config");
  if(cfg.contains("komiMean") && (cfg.contains("komiAuto") && cfg.getBool("komiAuto")))
    throw IOError("Must specify only one of komiMean=<komi value> or komiAuto=True in config");

  komiMean = cfg.contains("komiMean") ? cfg.getFloat("komiMean",Rules::MIN_USER_KOMI,Rules::MAX_USER_KOMI) : 7.5f;
  komiStdev = cfg.contains("komiStdev") ? cfg.getFloat("komiStdev",0.0f,60.0f) : 0.0f;
  handicapProb = cfg.contains("handicapProb") ? cfg.getDouble("handicapProb",0.0,1.0) : 0.0;
  handicapCompensateKomiProb = cfg.contains("handicapCompensateKomiProb") ? cfg.getDouble("handicapCompensateKomiProb",0.0,1.0) : 0.0;
  komiBigStdevProb = cfg.contains("komiBigStdevProb") ? cfg.getDouble("komiBigStdevProb",0.0,1.0) : 0.0;
  komiBigStdev = cfg.contains("komiBigStdev") ? cfg.getFloat("komiBigStdev",0.0f,60.0f) : 10.0f;
  komiBiggerStdevProb = cfg.contains("komiBiggerStdevProb") ? cfg.getDouble("komiBiggerStdevProb",0.0,1.0) : 0.0;
  komiBiggerStdev = cfg.contains("komiBiggerStdev") ? cfg.getFloat("komiBiggerStdev",0.0f,120.0f) : 30.0f;
  handicapKomiInterpZeroProb = cfg.contains("handicapKomiInterpZeroProb") ? cfg.getDouble("handicapKomiInterpZeroProb",0.0,1.0) : 0.0;
  sgfKomiInterpZeroProb = cfg.contains("sgfKomiInterpZeroProb") ? cfg.getDouble("sgfKomiInterpZeroProb",0.0,1.0) : 0.0;
  komiAuto = cfg.contains("komiAuto") ? cfg.getBool("komiAuto") : false;

  forkCompensateKomiProb = cfg.contains("forkCompensateKomiProb") ? cfg.getDouble("forkCompensateKomiProb",0.0,1.0) : handicapCompensateKomiProb;
  sgfCompensateKomiProb = cfg.contains("sgfCompensateKomiProb") ? cfg.getDouble("sgfCompensateKomiProb",0.0,1.0) : forkCompensateKomiProb;
  komiAllowIntegerProb = cfg.contains("komiAllowIntegerProb") ? cfg.getDouble("komiAllowIntegerProb",0.0,1.0) : 1.0;

  auto generateCumProbs = [](const vector<Sgf::PositionSample> poses, double lambda, double& effectiveSampleSize) {
    int64_t minInitialTurnNumber = 0;
    for(size_t i = 0; i<poses.size(); i++)
      minInitialTurnNumber = std::min(minInitialTurnNumber, poses[i].initialTurnNumber);

    vector<double> cumProbs;
    cumProbs.resize(poses.size());
    // Fill with uncumulative probs
    for(size_t i = 0; i<poses.size(); i++) {
      int64_t startTurn = poses[i].getCurrentTurnNumber() - minInitialTurnNumber;
      cumProbs[i] = exp(-startTurn * lambda) * poses[i].weight;
    }
    for(size_t i = 0; i<poses.size(); i++) {
      if(!(cumProbs[i] > -1e200 && cumProbs[i] < 1e200)) {
        throw StringError("startPos found bad unnormalized probability: " + Global::doubleToString(cumProbs[i]));
      }
    }

    // Compute ESS
    double sum = 0.0;
    double sumSq = 0.0;
    for(size_t i = 0; i<poses.size(); i++) {
      sum += cumProbs[i];
      sumSq += cumProbs[i]*cumProbs[i];
    }
    effectiveSampleSize = sum * sum / (sumSq + 1e-200);

    // Make cumulative
    for(size_t i = 1; i<poses.size(); i++)
      cumProbs[i] += cumProbs[i-1];

    return cumProbs;
  };

  startPosesProb = 0.0;
  if(cfg.contains("startPosesFromSgfDir")) {
    startPoses.clear();
    startPosCumProbs.clear();
    startPosesProb = cfg.getDouble("startPosesProb",0.0,1.0);

    vector<string> dirs = Global::split(cfg.getString("startPosesFromSgfDir"),',');
    vector<string> excludes = Global::split(cfg.contains("startPosesSgfExcludes") ? cfg.getString("startPosesSgfExcludes") : "",',');
    double startPosesLoadProb = cfg.getDouble("startPosesLoadProb",0.0,1.0);
    double startPosesTurnWeightLambda = cfg.getDouble("startPosesTurnWeightLambda",-10,10);

    vector<string> files;
    FileHelpers::collectSgfsFromDirs(dirs,files);
    std::set<Hash128> excludeHashes = Sgf::readExcludes(excludes);
    logger.write("Found " + Global::uint64ToString(files.size()) + " sgf files");
    logger.write("Loaded " + Global::uint64ToString(excludeHashes.size()) + " excludes");
    std::set<Hash128> uniqueHashes;
    std::function<void(Sgf::PositionSample&, const BoardHistory&, const string&)> posHandler = [startPosesLoadProb,this](
      Sgf::PositionSample& posSample, const BoardHistory& hist, const string& comments
    ) {
      (void)hist;
      (void)comments;
      if(rand.nextBool(startPosesLoadProb))
        startPoses.push_back(posSample);
    };
    int64_t numExcluded = 0;
    for(size_t i = 0; i<files.size(); i++) {
      Sgf* sgf = NULL;
      try {
        sgf = Sgf::loadFile(files[i]);
        if(contains(excludeHashes,sgf->hash))
          numExcluded += 1;
        else {
          bool hashComments = false;
          bool hashParent = false;
          bool flipIfPassOrWFirst = true;
          bool allowGameOver = false;
          sgf->iterAllUniquePositions(uniqueHashes, hashComments, hashParent, flipIfPassOrWFirst, allowGameOver, NULL, posHandler);
        }
      }
      catch(const StringError& e) {
        logger.write("Invalid SGF " + files[i] + ": " + e.what());
      }
      if(sgf != NULL)
        delete sgf;
    }
    logger.write("Kept " + Global::uint64ToString(startPoses.size()) + " start positions");
    logger.write("Excluded " + Global::int64ToString(numExcluded) + "/" + Global::uint64ToString(files.size()) + " sgf files");

    double ess = 0.0;
    startPosCumProbs = generateCumProbs(startPoses, startPosesTurnWeightLambda, ess);

    if(startPoses.size() <= 0) {
      logger.write("No start positions loaded, disabling start position logic");
      startPosesProb = 0;
    }
    else {
      logger.write("Cumulative unnormalized probability for start poses: " + Global::doubleToString(startPosCumProbs[startPoses.size()-1]));
      logger.write("Effective sample size for start poses: " + Global::doubleToString(ess));
    }
  }

  hintPosesProb = 0.0;
  if(cfg.contains("hintPosesDir")) {
    hintPoses.clear();
    hintPosCumProbs.clear();
    hintPosesProb = cfg.getDouble("hintPosesProb",0.0,1.0);

    vector<string> dirs = Global::split(cfg.getString("hintPosesDir"),',');

    vector<string> files;
    std::function<bool(const string&)> fileFilter = [](const string& fileName) {
      return
        Global::isSuffix(fileName,".hintposes.txt") ||
        Global::isSuffix(fileName,".startposes.txt") ||
        Global::isSuffix(fileName,".bookposes.txt");
    };
    for(int i = 0; i<dirs.size(); i++) {
      string dir = Global::trim(dirs[i]);
      if(dir.size() > 0)
        FileUtils::collectFiles(dir, fileFilter, files);
    }

    for(size_t i = 0; i<files.size(); i++) {
      vector<string> lines = FileUtils::readFileLines(files[i],'\n');
      for(size_t j = 0; j<lines.size(); j++) {
        string line = Global::trim(lines[j]);
        if(line.size() > 0) {
          try {
            Sgf::PositionSample posSample = Sgf::PositionSample::ofJsonLine(line);
            hintPoses.push_back(posSample);
          }
          catch(const StringError& err) {
            logger.write(string("ERROR parsing hintpos: ") + err.what());
          }
        }
      }
    }
    logger.write("Loaded " + Global::uint64ToString(hintPoses.size()) + " hint positions");

    double ess = 0.0;
    hintPosCumProbs = generateCumProbs(hintPoses, 0.0, ess);

    if(hintPoses.size() <= 0) {
      logger.write("No hint positions loaded, disabling hint position logic");
      hintPosesProb = 0;
    }
    else {
      logger.write("Cumulative unnormalized probability for hint poses: " + Global::doubleToString(hintPosCumProbs[hintPoses.size()-1]));
      logger.write("Effective sample size for hint poses: " + Global::doubleToString(ess));
    }
  }

  if(allowedBSizes.size() <= 0)
    throw IOError("bSizes or bSizesXY must have at least one value in " + cfg.getFileName());
  if(allowedBSizes.size() != allowedBSizeRelProbs.size())
    throw IOError("bSizes or bSizesXY and bSizeRelProbs must have same number of values in " + cfg.getFileName());

  minBoardXSize = allowedBSizes[0].first;
  minBoardYSize = allowedBSizes[0].second;
  maxBoardXSize = allowedBSizes[0].first;
  maxBoardYSize = allowedBSizes[0].second;
  for(const std::pair<int,int>& bSize : allowedBSizes) {
    minBoardXSize = std::min(minBoardXSize, bSize.first);
    minBoardYSize = std::min(minBoardYSize, bSize.second);
    maxBoardXSize = std::max(maxBoardXSize, bSize.first);
    maxBoardYSize = std::max(maxBoardYSize, bSize.second);
  }
  for(const Sgf::PositionSample& pos : hintPoses) {
    minBoardXSize = std::min(minBoardXSize, pos.board.x_size);
    minBoardYSize = std::min(minBoardYSize, pos.board.y_size);
    maxBoardXSize = std::max(maxBoardXSize, pos.board.x_size);
    maxBoardYSize = std::max(maxBoardYSize, pos.board.y_size);
  }

  noResultStdev = cfg.contains("noResultStdev") ? cfg.getDouble("noResultStdev",0.0,1.0) : 0.0;
  numExtraBlackFixed = cfg.contains("numExtraBlackFixed") ? cfg.getInt("numExtraBlackFixed",1,18) : 0;
  drawRandRadius = cfg.contains("drawRandRadius") ? cfg.getDouble("drawRandRadius",0.0,1.0) : 0.0;
}

GameInitializer::~GameInitializer()
{}

void GameInitializer::createGame(
  Board& board, Player& pla, BoardHistory& hist,
  ExtraBlackAndKomi& extraBlackAndKomi,
  const InitialPosition* initialPosition,
  const PlaySettings& playSettings,
  OtherGameProperties& otherGameProps,
  const Sgf::PositionSample* startPosSample
) {
  //Multiple threads will be calling this, and we have some mutable state such as rand.
  lock_guard<std::mutex> lock(createGameMutex);
  createGameSharedUnsynchronized(board,pla,hist,extraBlackAndKomi,initialPosition,playSettings,otherGameProps,startPosSample);
  if(noResultStdev != 0.0 || drawRandRadius != 0.0)
    throw StringError("GameInitializer::createGame called in a mode that doesn't support specifying noResultStdev or drawRandRadius");
}

void GameInitializer::createGame(
  Board& board, Player& pla, BoardHistory& hist,
  ExtraBlackAndKomi& extraBlackAndKomi,
  SearchParams& params,
  const InitialPosition* initialPosition,
  const PlaySettings& playSettings,
  OtherGameProperties& otherGameProps,
  const Sgf::PositionSample* startPosSample
) {
  //Multiple threads will be calling this, and we have some mutable state such as rand.
  lock_guard<std::mutex> lock(createGameMutex);
  createGameSharedUnsynchronized(board,pla,hist,extraBlackAndKomi,initialPosition,playSettings,otherGameProps,startPosSample);

  if(noResultStdev > 1e-30) {
    double mean = params.noResultUtilityForWhite;
    params.noResultUtilityForWhite = mean + noResultStdev * rand.nextGaussianTruncated(3.0);
    while(params.noResultUtilityForWhite < -1.0 || params.noResultUtilityForWhite > 1.0)
      params.noResultUtilityForWhite = mean + noResultStdev * rand.nextGaussianTruncated(3.0);
  }
  if(drawRandRadius > 1e-30) {
    double mean = params.drawEquivalentWinsForWhite;
    if(mean < 0.0 || mean > 1.0)
      throw StringError("GameInitializer: params.drawEquivalentWinsForWhite not within [0,1]: " + Global::doubleToString(mean));
    params.drawEquivalentWinsForWhite = mean + drawRandRadius * (rand.nextDouble() * 2 - 1);
    while(params.drawEquivalentWinsForWhite < 0.0 || params.drawEquivalentWinsForWhite > 1.0)
      params.drawEquivalentWinsForWhite = mean + drawRandRadius * (rand.nextDouble() * 2 - 1);
  }
}

Rules GameInitializer::randomizeScoringAndTaxRules(Rules rules, Rand& randToUse) const {
  rules.scoringRule = allowedScoringRules[randToUse.nextUInt((uint32_t)allowedScoringRules.size())];
  rules.taxRule = allowedTaxRules[randToUse.nextUInt((uint32_t)allowedTaxRules.size())];

  if(rules.scoringRule == Rules::SCORING_AREA)
    rules.hasButton = allowedButtons[randToUse.nextUInt((uint32_t)allowedButtons.size())];
  else
    rules.hasButton = false;

  return rules;
}

bool GameInitializer::isAllowedBSize(int xSize, int ySize) {
  if(!contains(allowedBSizes,std::make_pair(xSize,ySize)))
    return false;
  return true;
}

std::vector<std::pair<int,int>> GameInitializer::getAllowedBSizes() const {
  return allowedBSizes;
}
int GameInitializer::getMinBoardXSize() const {
  return minBoardXSize;
}
int GameInitializer::getMinBoardYSize() const {
  return minBoardYSize;
}
int GameInitializer::getMaxBoardXSize() const {
  return maxBoardXSize;
}
int GameInitializer::getMaxBoardYSize() const {
  return maxBoardYSize;
}

Rules GameInitializer::createRules() {
  lock_guard<std::mutex> lock(createGameMutex);
  return createRulesUnsynchronized();
}

Rules GameInitializer::createRulesUnsynchronized() {
  Rules rules;
  rules.koRule = allowedKoRules[rand.nextUInt((uint32_t)allowedKoRules.size())];
  rules.scoringRule = allowedScoringRules[rand.nextUInt((uint32_t)allowedScoringRules.size())];
  rules.taxRule = allowedTaxRules[rand.nextUInt((uint32_t)allowedTaxRules.size())];
  rules.multiStoneSuicideLegal = allowedMultiStoneSuicideLegals[rand.nextUInt((uint32_t)allowedMultiStoneSuicideLegals.size())];

  if(rules.scoringRule == Rules::SCORING_AREA)
    rules.hasButton = allowedButtons[rand.nextUInt((uint32_t)allowedButtons.size())];
  else
    rules.hasButton = false;
  return rules;
}

void GameInitializer::createGameSharedUnsynchronized(
  Board& board, Player& pla, BoardHistory& hist,
  ExtraBlackAndKomi& extraBlackAndKomi,
  const InitialPosition* initialPosition,
  const PlaySettings& playSettings,
  OtherGameProperties& otherGameProps,
  const Sgf::PositionSample* startPosSample
) {
  if(initialPosition != NULL) {
    board = initialPosition->board;
    hist = initialPosition->hist;
    pla = initialPosition->pla;

    //No handicap when starting from an initial position.
    double thisHandicapProb = 0.0;
    extraBlackAndKomi = PlayUtils::chooseExtraBlackAndKomi(
      hist.rules.komi, komiStdev, komiAllowIntegerProb,
      thisHandicapProb, numExtraBlackFixed,
      komiBigStdevProb, komiBigStdev,
      komiBiggerStdevProb, komiBiggerStdev,
      sqrt(board.x_size*board.y_size), rand
    );
    assert(extraBlackAndKomi.extraBlack == 0);
    PlayUtils::setKomiWithNoise(extraBlackAndKomi, hist, rand);
    otherGameProps.isSgfPos = false;
    otherGameProps.isHintPos = false;
    otherGameProps.allowPolicyInit = false; //On fork positions, don't play extra moves at start
    otherGameProps.isFork = true;
    otherGameProps.isHintFork = initialPosition->isHintFork;
    otherGameProps.hintLoc = Board::NULL_LOC;
    otherGameProps.hintTurn = initialPosition->isHintFork ? (int)hist.moveHistory.size() : -1;
    otherGameProps.trainingWeight = initialPosition->trainingWeight;
    extraBlackAndKomi.makeGameFair = rand.nextBool(forkCompensateKomiProb);
    extraBlackAndKomi.makeGameFairForEmptyBoard = false;
    extraBlackAndKomi.interpZero = false;
    return;
  }

  double makeGameFairProb = 0.0;

  int bSizeIdx = rand.nextUInt(allowedBSizeRelProbs.data(),allowedBSizeRelProbs.size());
  Rules rules = createRulesUnsynchronized();

  const Sgf::PositionSample* posSample = NULL;
  if(startPosSample != NULL)
    posSample = startPosSample;

  if(posSample == NULL) {
    if(startPosesProb > 0 && rand.nextBool(startPosesProb)) {
      assert(startPoses.size() > 0);
      size_t r = rand.nextIndexCumulative(startPosCumProbs.data(),startPosCumProbs.size());
      assert(r < startPosCumProbs.size());
      posSample = &(startPoses[r]);
    }
    else if(hintPosesProb > 0 && rand.nextBool(hintPosesProb)) {
      assert(hintPoses.size() > 0);
      size_t r = rand.nextIndexCumulative(hintPosCumProbs.data(),hintPosCumProbs.size());
      assert(r < hintPosCumProbs.size());
      posSample = &(hintPoses[r]);
    }
  }

  if(posSample != NULL) {
    const Sgf::PositionSample& startPos = *posSample;
    board = startPos.board;
    pla = startPos.nextPla;
    hist.clear(board,pla,rules,0);
    hist.setInitialTurnNumber(startPos.initialTurnNumber);
    Loc hintLoc = startPos.hintLoc;
    testAssert(startPos.moves.size() < 0xFFFFFF);
    for(size_t i = 0; i<startPos.moves.size(); i++) {
      bool isLegal = hist.isLegal(board,startPos.moves[i].loc,startPos.moves[i].pla);
      //Makes a best effort to still use the position, stopping if we hit an illegal move. It's possible
      //we hit this because of rules differences making a superko move or self-capture illegal, for example.
      if(!isLegal || hist.isGameFinished) {
        //If we stop due to illegality, it doesn't make sense to still use the hintLoc
        hintLoc = Board::NULL_LOC;
        break;
      }
      hist.makeBoardMoveAssumeLegal(board,startPos.moves[i].loc,startPos.moves[i].pla,NULL);
      pla = getOpp(startPos.moves[i].pla);
    }

    //No handicap when starting from a sampled position.
    double thisHandicapProb = 0.0;
    extraBlackAndKomi = PlayUtils::chooseExtraBlackAndKomi(
      komiMean, komiStdev, komiAllowIntegerProb,
      thisHandicapProb, numExtraBlackFixed,
      komiBigStdevProb, komiBigStdev,
      komiBiggerStdevProb, komiBiggerStdev,
      sqrt(board.x_size*board.y_size), rand
    );
    PlayUtils::setKomiWithNoise(extraBlackAndKomi, hist, rand);

    otherGameProps.isSgfPos = hintLoc == Board::NULL_LOC;
    otherGameProps.isHintPos = hintLoc != Board::NULL_LOC;
    otherGameProps.allowPolicyInit = hintLoc == Board::NULL_LOC; //On sgf positions, do allow extra moves at start
    otherGameProps.isFork = false;
    otherGameProps.isHintFork = false;
    otherGameProps.hintLoc = hintLoc;
    otherGameProps.hintTurn = (int)hist.moveHistory.size();
    otherGameProps.hintPosHash = board.pos_hash;
    otherGameProps.trainingWeight = startPos.trainingWeight;
    makeGameFairProb = sgfCompensateKomiProb;
    extraBlackAndKomi.interpZero = sgfKomiInterpZeroProb > 0 ? rand.nextBool(sgfKomiInterpZeroProb) : false;
  }
  else {
    int xSize = allowedBSizes[bSizeIdx].first;
    int ySize = allowedBSizes[bSizeIdx].second;
    board = Board(xSize,ySize);
    pla = P_BLACK;
    hist.clear(board,pla,rules,0);

    extraBlackAndKomi = PlayUtils::chooseExtraBlackAndKomi(
      komiMean, komiStdev, komiAllowIntegerProb,
      handicapProb, numExtraBlackFixed,
      komiBigStdevProb, komiBigStdev,
      komiBiggerStdevProb, komiBiggerStdev,
      sqrt(board.x_size*board.y_size), rand
    );
    PlayUtils::setKomiWithNoise(extraBlackAndKomi, hist, rand);

    otherGameProps.isSgfPos = false;
    otherGameProps.isHintPos = false;
    otherGameProps.allowPolicyInit = true; //Handicap and regular games do allow policy init
    otherGameProps.isFork = false;
    otherGameProps.isHintFork = false;
    otherGameProps.hintLoc = Board::NULL_LOC;
    otherGameProps.hintTurn = -1;
    otherGameProps.trainingWeight = 1.0;
    makeGameFairProb = extraBlackAndKomi.extraBlack > 0 ? handicapCompensateKomiProb : 0.0;
    extraBlackAndKomi.interpZero = (handicapKomiInterpZeroProb > 0 && extraBlackAndKomi.extraBlack > 0) ? rand.nextBool(handicapKomiInterpZeroProb) : false;
  }

  double asymmetricProb = (extraBlackAndKomi.extraBlack > 0) ? playSettings.handicapAsymmetricPlayoutProb : playSettings.normalAsymmetricPlayoutProb;
  if(asymmetricProb > 0 && rand.nextBool(asymmetricProb)) {
    assert(playSettings.maxAsymmetricRatio >= 1.0);
    double maxNumDoublings = log(playSettings.maxAsymmetricRatio) / log(2.0);
    double numDoublings = rand.nextDouble(maxNumDoublings);
    if(extraBlackAndKomi.extraBlack > 0 || rand.nextBool(0.5)) {
      otherGameProps.playoutDoublingAdvantagePla = C_WHITE;
      otherGameProps.playoutDoublingAdvantage = numDoublings;
    }
    else {
      otherGameProps.playoutDoublingAdvantagePla = C_BLACK;
      otherGameProps.playoutDoublingAdvantage = numDoublings;
    }
    makeGameFairProb = std::max(makeGameFairProb,playSettings.minAsymmetricCompensateKomiProb);
  }

  if(komiAuto) {
    if(makeGameFairProb > 0.0)
      extraBlackAndKomi.makeGameFair = rand.nextBool(makeGameFairProb);
    extraBlackAndKomi.makeGameFairForEmptyBoard = !extraBlackAndKomi.makeGameFair;
  }
  else {
    if(makeGameFairProb > 0.0)
      extraBlackAndKomi.makeGameFair = rand.nextBool(makeGameFairProb);
    extraBlackAndKomi.makeGameFairForEmptyBoard = false;
  }
}

//----------------------------------------------------------------------------------------------------------

MatchPairer::MatchPairer(
  ConfigParser& cfg,
  int nBots,
  const vector<string>& bNames,
  const vector<NNEvaluator*>& nEvals,
  const vector<SearchParams>& bParamss,
  const std::vector<std::pair<int,int>>& matchups,
  int64_t numGames
)
  :numBots(nBots),
   botNames(bNames),
   nnEvals(nEvals),
   baseParamss(bParamss),
   matchupsPerRound(matchups),
   nextMatchups(),
   rand(),
   numGamesStartedSoFar(0),
   numGamesTotal(numGames),
   logGamesEvery(),
   getMatchupMutex()
{
  assert(botNames.size() == numBots);
  assert(nnEvals.size() == numBots);
  assert(baseParamss.size() == numBots);

  if(matchupsPerRound.size() <= 0)
    throw StringError("MatchPairer: no matchups specified");
  if(matchupsPerRound.size() > 0xFFFFFF)
    throw StringError("MatchPairer: too many matchups");

  logGamesEvery = cfg.getInt64("logGamesEvery",1,1000000);
}

MatchPairer::~MatchPairer()
{}

int64_t MatchPairer::getNumGamesTotalToGenerate() const {
  return numGamesTotal;
}

bool MatchPairer::getMatchup(
  BotSpec& botSpecB, BotSpec& botSpecW, Logger& logger
)
{
  std::lock_guard<std::mutex> lock(getMatchupMutex);

  if(numGamesStartedSoFar >= numGamesTotal)
    return false;

  numGamesStartedSoFar += 1;

  if(numGamesStartedSoFar % logGamesEvery == 0)
    logger.write("Started " + Global::int64ToString(numGamesStartedSoFar) + " games");
  int64_t logNNEvery = logGamesEvery*100 > 1000 ? logGamesEvery*100 : 1000;
  if(numGamesStartedSoFar % logNNEvery == 0) {
    for(int i = 0; i<nnEvals.size(); i++) {
      if(nnEvals[i] != NULL) {
        logger.write(nnEvals[i]->getModelFileName());
        logger.write("NN rows: " + Global::int64ToString(nnEvals[i]->numRowsProcessed()));
        logger.write("NN batches: " + Global::int64ToString(nnEvals[i]->numBatchesProcessed()));
        logger.write("NN avg batch size: " + Global::doubleToString(nnEvals[i]->averageProcessedBatchSize()));
      }
    }
  }

  pair<int,int> matchup = getMatchupPairUnsynchronized();

  botSpecB.botIdx = matchup.first;
  botSpecB.botName = botNames[matchup.first];
  botSpecB.nnEval = nnEvals[matchup.first];
  botSpecB.baseParams = baseParamss[matchup.first];

  botSpecW.botIdx = matchup.second;
  botSpecW.botName = botNames[matchup.second];
  botSpecW.nnEval = nnEvals[matchup.second];
  botSpecW.baseParams = baseParamss[matchup.second];

  return true;
}

pair<int,int> MatchPairer::getMatchupPairUnsynchronized() {
  if(nextMatchups.size() <= 0) {
    if(numBots == 0)
      throw StringError("MatchPairer::getMatchupPairUnsynchronized: no bots to match up");

    //Append all matches for the next round
    nextMatchups.clear();
    nextMatchups.insert(nextMatchups.begin(), matchupsPerRound.begin(), matchupsPerRound.end());

    //Shuffle
    for(int i = (int)nextMatchups.size()-1; i >= 1; i--) {
      int j = (int)rand.nextUInt(i+1);
      pair<int,int> tmp = nextMatchups[i];
      nextMatchups[i] = nextMatchups[j];
      nextMatchups[j] = tmp;
    }
  }

  pair<int,int> matchup = nextMatchups.back();
  nextMatchups.pop_back();

  return matchup;
}

//----------------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------------

static void failIllegalMove(Search* bot, Logger& logger, Board board, Loc loc) {
  ostringstream sout;
  sout << "Bot returned null location or illegal move!?!" << "\n";
  sout << board << "\n";
  sout << bot->getRootBoard() << "\n";
  sout << "Pla: " << PlayerIO::playerToString(bot->getRootPla()) << "\n";
  sout << "Loc: " << Location::toString(loc,bot->getRootBoard()) << "\n";
  logger.write(sout.str());
  bot->getRootBoard().checkConsistency();
  ASSERT_UNREACHABLE;
}

static void logSearch(Search* bot, Logger& logger, Loc loc, OtherGameProperties otherGameProps) {
  ostringstream sout;
  Board::printBoard(sout, bot->getRootBoard(), loc, &(bot->getRootHist().moveHistory));
  sout << "\n";
  sout << "Rules: " << bot->getRootHist().rules << "\n";
  sout << "Root visits: " << bot->getRootVisits() << "\n";
  if(otherGameProps.hintLoc != Board::NULL_LOC &&
     otherGameProps.hintTurn == bot->getRootHist().moveHistory.size() &&
     otherGameProps.hintPosHash == bot->getRootBoard().pos_hash) {
    sout << "HintLoc " << Location::toString(otherGameProps.hintLoc,bot->getRootBoard()) << "\n";
  }
  sout << "Policy surprise " << bot->getPolicySurprise() << "\n";
  sout << "Raw WL " << bot->getRootRawNNValuesRequireSuccess().winLossValue << "\n";
  sout << "PV: ";
  bot->printPV(sout, bot->rootNode, 25);
  sout << "\n";
  sout << "Tree:\n";
  bot->printTree(sout, bot->rootNode, PrintTreeOptions().maxDepth(1).maxChildrenToShow(10),P_WHITE);

  logger.write(sout.str());
}

static Loc chooseRandomForkingMove(const NNOutput* nnOutput, const Board& board, const BoardHistory& hist, Player pla, Rand& gameRand, Loc banMove) {
  double r = gameRand.nextDouble();
  bool allowPass = true;
  //70% of the time, do a random temperature 1 policy move
  if(r < 0.70)
    return PlayUtils::chooseRandomPolicyMove(nnOutput, board, hist, pla, gameRand, 1.0, allowPass, banMove);
  //25% of the time, do a random temperature 2 policy move
  else if(r < 0.95)
    return PlayUtils::chooseRandomPolicyMove(nnOutput, board, hist, pla, gameRand, 2.0, allowPass, banMove);
  //5% of the time, do a random legal move
  else
    return PlayUtils::chooseRandomLegalMove(board, hist, pla, gameRand, banMove);
}

void Play::extractPolicyTarget(
  vector<PolicyTargetMove>& buf,
  const Search* toMoveBot,
  const SearchNode* node,
  vector<Loc>& locsBuf,
  vector<double>& playSelectionValuesBuf
) {
  double scaleMaxToAtLeast = 10.0;

  assert(node != NULL);
  assert(!toMoveBot->searchParams.rootSymmetryPruning);
  bool allowDirectPolicyMoves = false;
  bool success = toMoveBot->getPlaySelectionValues(*node,locsBuf,playSelectionValuesBuf,NULL,scaleMaxToAtLeast,allowDirectPolicyMoves);
  testAssert(success);
  (void)success; //Avoid warning when asserts are disabled

  assert(locsBuf.size() == playSelectionValuesBuf.size());
  assert(locsBuf.size() <= toMoveBot->rootBoard.x_size * toMoveBot->rootBoard.y_size + 1);

  //Make sure we don't overflow int16
  double maxValue = 0.0;
  for(int moveIdx = 0; moveIdx<locsBuf.size(); moveIdx++) {
    double value = playSelectionValuesBuf[moveIdx];
    testAssert(value >= 0.0);
    if(value > maxValue)
      maxValue = value;
  }

  double factor = 1.0;
  if(maxValue > 30000.0)
    factor = 30000.0 / maxValue;

  for(int moveIdx = 0; moveIdx<locsBuf.size(); moveIdx++) {
    double value = playSelectionValuesBuf[moveIdx] * factor;
    assert(value <= 30001.0);
    buf.push_back(PolicyTargetMove(locsBuf[moveIdx],(int16_t)round(value)));
  }
}

static void extractValueTargets(ValueTargets& buf, const Search* toMoveBot, const SearchNode* node) {
  ReportedSearchValues values;
  bool success = toMoveBot->getNodeValues(node,values);
  testAssert(success);
  (void)success; //Avoid warning when asserts are disabled

  buf.win = (float)values.winValue;
  buf.loss = (float)values.lossValue;
  buf.noResult = (float)values.noResultValue;
  buf.score = (float)values.expectedScore;
}

static void extractQValueTargets(
  vector<QValueTargetMove>& buf,
  const Search* toMoveBot,
  const SearchNode* node
) {
  ConstSearchNodeChildrenReference children = node->getChildren();
  int numChildren = children.iterateAndCountChildren();
  for(int childIdx = 0; childIdx < numChildren; childIdx++) {
    const SearchChildPointer& childPtr = children[childIdx];
    const SearchNode* child = childPtr.getIfAllocated();

    if(child == NULL)
      continue;

    ReportedSearchValues values;
    bool success = toMoveBot->getNodeValues(child, values);
    if(!success)
      continue;
    if(values.visits <= 0)
      continue;

    Loc moveLoc = childPtr.getMoveLoc();
    buf.push_back(
      QValueTargetMove(
        moveLoc,
        (float)values.winLossValue,
        (float)values.expectedScore,
        values.visits
      )
    );
  }
}

static NNRawStats computeNNRawStats(const Search* bot, const Board& board, const BoardHistory& hist, Player pla) {
  NNResultBuf buf;
  MiscNNInputParams nnInputParams;
  nnInputParams.drawEquivalentWinsForWhite = bot->searchParams.drawEquivalentWinsForWhite;
  Board b = board;
  bot->nnEvaluator->evaluate(b,hist,pla,nnInputParams,buf,false,false);
  NNOutput& nnOutput = *(buf.result);

  NNRawStats nnRawStats;
  nnRawStats.whiteWinLoss = nnOutput.whiteWinProb - nnOutput.whiteLossProb;
  nnRawStats.whiteScoreMean = nnOutput.whiteScoreMean;
  {
    double entropy = 0.0;
    int policySize = NNPos::getPolicySize(nnOutput.nnXLen,nnOutput.nnYLen);
    for(int pos = 0; pos<policySize; pos++) {
      double prob = nnOutput.policyProbs[pos];
      if(prob >= 1e-30)
        entropy += -prob * log(prob);
    }
    nnRawStats.policyEntropy = entropy;
  }
  return nnRawStats;
}

//Recursively walk non-root-node subtree under node recording positions that have enough visits
//We also only record positions where the player to move made best moves along the tree so far.
//Does NOT walk down branches of excludeLoc0 and excludeLoc1 - these are used to avoid writing
//subtree positions for branches that we are about to actually play or do a forked sideposition search on.
static void recordTreePositionsRec(
  FinishedGameData* gameData,
  const Board& board, const BoardHistory& hist, Player pla,
  const Search* toMoveBot,
  const SearchNode* node, int depth, int maxDepth, bool plaAlwaysBest, bool oppAlwaysBest,
  int64_t minVisitsAtNode, float recordTreeTargetWeight,
  int numNeuralNetChangesSoFar,
  vector<Loc>& locsBuf, vector<double>& playSelectionValuesBuf,
  Loc excludeLoc0, Loc excludeLoc1
) {
  ConstSearchNodeChildrenReference children = node->getChildren();
  int numChildren = children.iterateAndCountChildren();

  if(numChildren <= 0)
    return;

  if(plaAlwaysBest && node != toMoveBot->rootNode) {
    SidePosition* sp = new SidePosition(board,hist,pla,numNeuralNetChangesSoFar);
    Play::extractPolicyTarget(sp->policyTarget, toMoveBot, node, locsBuf, playSelectionValuesBuf);
    extractValueTargets(sp->whiteValueTargets, toMoveBot, node);
    extractQValueTargets(sp->whiteQValueTargets.targets, toMoveBot, node);

    double policySurprise = 0.0, policyEntropy = 0.0, searchEntropy = 0.0;
    bool success = toMoveBot->getPolicySurpriseAndEntropy(policySurprise, searchEntropy, policyEntropy, node);
    testAssert(success);
    (void)success; //Avoid warning when asserts are disabled
    sp->policySurprise = policySurprise;
    sp->policyEntropy = policyEntropy;
    sp->searchEntropy = searchEntropy;

    sp->nnRawStats = computeNNRawStats(toMoveBot, board, hist, pla);
    sp->targetWeight = recordTreeTargetWeight;
    sp->unreducedNumVisits = toMoveBot->getRootVisits();
    gameData->sidePositions.push_back(sp);
  }

  if(depth >= maxDepth)
    return;

  //Best child is the one with the largest number of visits, find it
  int bestChildIdx = 0;
  int64_t bestChildVisits = 0;
  for(int i = 1; i<numChildren; i++) {
    const SearchNode* child = children[i].getIfAllocated();
    while(child->statsLock.test_and_set(std::memory_order_acquire));
    int64_t numVisits = child->stats.visits;
    child->statsLock.clear(std::memory_order_release);
    if(numVisits > bestChildVisits) {
      bestChildVisits = numVisits;
      bestChildIdx = i;
    }
  }

  for(int i = 0; i<numChildren; i++) {
    bool newPlaAlwaysBest = oppAlwaysBest;
    bool newOppAlwaysBest = plaAlwaysBest && i == bestChildIdx;

    if(!newPlaAlwaysBest && !newOppAlwaysBest)
      continue;

    const SearchNode* child = children[i].getIfAllocated();
    Loc moveLoc = children[i].getMoveLoc();
    if(moveLoc == excludeLoc0 || moveLoc == excludeLoc1)
      continue;

    int64_t numVisits = child->stats.visits.load(std::memory_order_acquire);
    if(numVisits < minVisitsAtNode)
      continue;

    if(hist.isLegal(board, moveLoc, pla)) {
      Board copy = board;
      BoardHistory histCopy = hist;
      histCopy.makeBoardMoveAssumeLegal(copy, moveLoc, pla, NULL);
      Player nextPla = getOpp(pla);
      recordTreePositionsRec(
        gameData,
        copy,histCopy,nextPla,
        toMoveBot,
        child,depth+1,maxDepth,newPlaAlwaysBest,newOppAlwaysBest,
        minVisitsAtNode,recordTreeTargetWeight,
        numNeuralNetChangesSoFar,
        locsBuf,playSelectionValuesBuf,
        Board::NULL_LOC,Board::NULL_LOC
      );
    }
  }
}

//Top-level caller for recursive func
static void recordTreePositions(
  FinishedGameData* gameData,
  const Board& board, const BoardHistory& hist, Player pla,
  const Search* toMoveBot,
  int64_t minVisitsAtNode, float recordTreeTargetWeight,
  int numNeuralNetChangesSoFar,
  vector<Loc>& locsBuf, vector<double>& playSelectionValuesBuf,
  Loc excludeLoc0, Loc excludeLoc1
) {
  assert(toMoveBot->rootBoard.pos_hash == board.pos_hash);
  assert(toMoveBot->rootHistory.moveHistory.size() == hist.moveHistory.size());
  assert(toMoveBot->rootPla == pla);
  assert(toMoveBot->rootNode != NULL);
  //Don't go too deep recording extra positions
  int maxDepth = 5;
  recordTreePositionsRec(
    gameData,
    board,hist,pla,
    toMoveBot,
    toMoveBot->rootNode, 0, maxDepth, true, true,
    minVisitsAtNode, recordTreeTargetWeight,
    numNeuralNetChangesSoFar,
    locsBuf,playSelectionValuesBuf,
    excludeLoc0,excludeLoc1
  );
}


struct SearchLimitsThisMove {
  bool doAlterVisitsPlayouts;
  int64_t numAlterVisits;
  int64_t numAlterPlayouts;
  bool clearBotBeforeSearchThisMove;
  bool removeRootNoise;
  float targetWeight;

  //Note: these two behave slightly differently than the ones in searchParams - derived from OtherGameProperties
  //game, they make the playouts *actually* vary instead of only making the neural net think they do.
  double playoutDoublingAdvantage;
  Player playoutDoublingAdvantagePla;

  Loc hintLoc;
};

static SearchLimitsThisMove getSearchLimitsThisMove(
  const Search* toMoveBot, Player pla, const PlaySettings& playSettings, Rand& gameRand,
  const vector<double>& historicalMctsWinLossValues,
  bool clearBotBeforeSearch,
  const OtherGameProperties& otherGameProps
) {
  bool doAlterVisitsPlayouts = false;
  int64_t numAlterVisits = toMoveBot->searchParams.maxVisits;
  int64_t numAlterPlayouts = toMoveBot->searchParams.maxPlayouts;
  bool clearBotBeforeSearchThisMove = clearBotBeforeSearch;
  bool removeRootNoise = false;
  float targetWeight = 1.0f;
  double playoutDoublingAdvantage = 0.0;
  Player playoutDoublingAdvantagePla = C_EMPTY;
  Loc hintLoc = Board::NULL_LOC;
  double cheapSearchProb = playSettings.cheapSearchProb;

  const BoardHistory& hist = toMoveBot->getRootHist();
  if(otherGameProps.hintLoc != Board::NULL_LOC) {
    if(otherGameProps.hintTurn == hist.moveHistory.size() &&
       otherGameProps.hintPosHash == toMoveBot->getRootBoard().pos_hash) {
      hintLoc = otherGameProps.hintLoc;
      doAlterVisitsPlayouts = true;
      double cap = (double)((int64_t)1L << 50);
      numAlterVisits = (int64_t)ceil(std::min(cap, numAlterVisits * 4.0));
      numAlterPlayouts = (int64_t)ceil(std::min(cap, numAlterPlayouts * 4.0));
    }
  }
  //For the first few turns after a hint move or fork, reduce the probability of cheap search
  if((otherGameProps.hintLoc != Board::NULL_LOC || otherGameProps.isHintFork) && otherGameProps.hintTurn + 6 > hist.moveHistory.size()) {
    cheapSearchProb *= 0.5;
  }


  if(hintLoc == Board::NULL_LOC && cheapSearchProb > 0.0 && gameRand.nextBool(cheapSearchProb)) {
    if(playSettings.cheapSearchVisits <= 0)
      throw StringError("playSettings.cheapSearchVisits <= 0");
    if(playSettings.cheapSearchVisits > toMoveBot->searchParams.maxVisits ||
       playSettings.cheapSearchVisits > toMoveBot->searchParams.maxPlayouts)
      throw StringError("playSettings.cheapSearchVisits > maxVisits and/or maxPlayouts");

    doAlterVisitsPlayouts = true;
    numAlterVisits = std::min(numAlterVisits,(int64_t)playSettings.cheapSearchVisits);
    numAlterPlayouts = std::min(numAlterPlayouts,(int64_t)playSettings.cheapSearchVisits);
    targetWeight *= playSettings.cheapSearchTargetWeight;

    //If not recording cheap searches, do a few more things
    if(playSettings.cheapSearchTargetWeight <= 0.0) {
      clearBotBeforeSearchThisMove = false;
      removeRootNoise = true;
    }
  }
  else if(hintLoc == Board::NULL_LOC && playSettings.reduceVisits) {
    if(playSettings.reducedVisitsMin <= 0)
      throw StringError("playSettings.reducedVisitsMin <= 0");
    if(playSettings.reducedVisitsMin > toMoveBot->searchParams.maxVisits ||
       playSettings.reducedVisitsMin > toMoveBot->searchParams.maxPlayouts)
      throw StringError("playSettings.reducedVisitsMin > maxVisits and/or maxPlayouts");

    if(historicalMctsWinLossValues.size() >= playSettings.reduceVisitsThresholdLookback) {
      double minWinLossValue = 1e20;
      double maxWinLossValue = -1e20;
      for(int j = 0; j<playSettings.reduceVisitsThresholdLookback; j++) {
        double winLossValue = historicalMctsWinLossValues[historicalMctsWinLossValues.size()-1-j];
        if(winLossValue < minWinLossValue)
          minWinLossValue = winLossValue;
        if(winLossValue > maxWinLossValue)
          maxWinLossValue = winLossValue;
      }
      assert(playSettings.reduceVisitsThreshold >= 0.0);
      double signedMostExtreme = std::max(minWinLossValue,-maxWinLossValue);
      assert(signedMostExtreme <= 1.000001);
      if(signedMostExtreme > 1.0)
        signedMostExtreme = 1.0;
      double amountThrough = signedMostExtreme - playSettings.reduceVisitsThreshold;
      if(amountThrough > 0) {
        double proportionThrough = amountThrough / (1.0 - playSettings.reduceVisitsThreshold);
        assert(proportionThrough >= 0.0 && proportionThrough <= 1.0);
        double visitReductionProp = proportionThrough * proportionThrough;
        doAlterVisitsPlayouts = true;
        numAlterVisits = (int64_t)round(numAlterVisits + visitReductionProp * ((double)playSettings.reducedVisitsMin - (double)numAlterVisits));
        numAlterPlayouts = (int64_t)round(numAlterPlayouts + visitReductionProp * ((double)playSettings.reducedVisitsMin - (double)numAlterPlayouts));
        targetWeight = (float)(targetWeight + visitReductionProp * (playSettings.reducedVisitsWeight - targetWeight));
        numAlterVisits = std::max(numAlterVisits,(int64_t)playSettings.reducedVisitsMin);
        numAlterPlayouts = std::max(numAlterPlayouts,(int64_t)playSettings.reducedVisitsMin);
      }
    }
  }

  if(otherGameProps.playoutDoublingAdvantage != 0.0 && otherGameProps.playoutDoublingAdvantagePla != C_EMPTY) {
    assert(pla == otherGameProps.playoutDoublingAdvantagePla || getOpp(pla) == otherGameProps.playoutDoublingAdvantagePla);

    playoutDoublingAdvantage = otherGameProps.playoutDoublingAdvantage;
    playoutDoublingAdvantagePla = otherGameProps.playoutDoublingAdvantagePla;

    double factor = pow(2.0, otherGameProps.playoutDoublingAdvantage);
    if(pla == otherGameProps.playoutDoublingAdvantagePla)
      factor = 2.0 * (factor / (factor + 1.0));
    else
      factor = 2.0 * (1.0 / (factor + 1.0));


    doAlterVisitsPlayouts = true;
    //Set this back to true - we need to always clear the search if we are doing asymmetric playouts
    clearBotBeforeSearchThisMove = true;
    numAlterVisits = (int64_t)round(numAlterVisits * factor);
    numAlterPlayouts = (int64_t)round(numAlterPlayouts * factor);

    //Hardcoded limit here to ensure sanity
    if(numAlterVisits < 5)
      throw StringError("ERROR: asymmetric playout doubling resulted in fewer than 5 visits");
    if(numAlterPlayouts < 5)
      throw StringError("ERROR: asymmetric playout doubling resulted in fewer than 5 playouts");
  }

  SearchLimitsThisMove limits;
  limits.doAlterVisitsPlayouts = doAlterVisitsPlayouts;
  limits.numAlterVisits = numAlterVisits;
  limits.numAlterPlayouts = numAlterPlayouts;
  limits.clearBotBeforeSearchThisMove = clearBotBeforeSearchThisMove;
  limits.removeRootNoise = removeRootNoise;
  limits.targetWeight = targetWeight;
  limits.playoutDoublingAdvantage = playoutDoublingAdvantage;
  limits.playoutDoublingAdvantagePla = playoutDoublingAdvantagePla;
  limits.hintLoc = hintLoc;
  return limits;
}

//Returns the move chosen
static Loc runBotWithLimits(
  Search* toMoveBot, Player pla, const PlaySettings& playSettings,
  const SearchLimitsThisMove& limits
) {
  if(limits.clearBotBeforeSearchThisMove)
    toMoveBot->clearSearch();

  Loc loc;

  //HACK - Disable LCB for making the move (it will still affect the policy target gen)
  bool lcb = toMoveBot->searchParams.useLcbForSelection;
  if(playSettings.forSelfPlay) {
    toMoveBot->searchParams.useLcbForSelection = false;
  }

  if(limits.doAlterVisitsPlayouts) {
    assert(limits.numAlterVisits > 0);
    assert(limits.numAlterPlayouts > 0);
    SearchParams oldParams = toMoveBot->searchParams;

    toMoveBot->searchParams.maxVisits = limits.numAlterVisits;
    toMoveBot->searchParams.maxPlayouts = limits.numAlterPlayouts;
    if(limits.removeRootNoise) {
      //Note - this is slightly sketchy to set the params directly. This works because
      //some of the parameters like FPU are basically stateless and will just affect future playouts
      //and because even stateful effects like rootNoiseEnabled and rootPolicyTemperature only affect
      //the root so when we step down in the tree we get a fresh start.
      toMoveBot->searchParams.rootNoiseEnabled = false;
      toMoveBot->searchParams.rootPolicyTemperature = 1.0;
      toMoveBot->searchParams.rootPolicyTemperatureEarly = 1.0;
      toMoveBot->searchParams.rootFpuLossProp = toMoveBot->searchParams.fpuLossProp;
      toMoveBot->searchParams.rootFpuReductionMax = toMoveBot->searchParams.fpuReductionMax;
      toMoveBot->searchParams.rootDesiredPerChildVisitsCoeff = 0.0;
      toMoveBot->searchParams.rootNumSymmetriesToSample = 1;
    }
    if(limits.playoutDoublingAdvantagePla != C_EMPTY) {
      toMoveBot->searchParams.playoutDoublingAdvantagePla = limits.playoutDoublingAdvantagePla;
      toMoveBot->searchParams.playoutDoublingAdvantage = limits.playoutDoublingAdvantage;
    }

    //If we cleared the search, do a very short search first to get a good dynamic score utility center
    if(limits.clearBotBeforeSearchThisMove && toMoveBot->searchParams.maxVisits > 10 && toMoveBot->searchParams.maxPlayouts > 10) {
      int64_t oldMaxVisits = toMoveBot->searchParams.maxVisits;
      toMoveBot->searchParams.maxVisits = 10;
      toMoveBot->runWholeSearchAndGetMove(pla);
      toMoveBot->searchParams.maxVisits = oldMaxVisits;
    }

    if(limits.hintLoc != Board::NULL_LOC) {
      assert(limits.clearBotBeforeSearchThisMove);
      //This will actually forcibly clear the search
      toMoveBot->setRootHintLoc(limits.hintLoc);
    }

    loc = toMoveBot->runWholeSearchAndGetMove(pla);

    if(limits.hintLoc != Board::NULL_LOC)
      toMoveBot->setRootHintLoc(Board::NULL_LOC);

    toMoveBot->searchParams = oldParams;
  }
  else {
    assert(!limits.removeRootNoise);
    loc = toMoveBot->runWholeSearchAndGetMove(pla);
  }

  //HACK - restore LCB so that it affects policy target gen
  if(playSettings.forSelfPlay) {
    toMoveBot->searchParams.useLcbForSelection = lcb;
  }

  return loc;
}


//Run a game between two bots. It is OK if both bots are the same bot.
FinishedGameData* Play::runGame(
  const Board& startBoard, Player pla, const BoardHistory& startHist, ExtraBlackAndKomi extraBlackAndKomi,
  MatchPairer::BotSpec& botSpecB, MatchPairer::BotSpec& botSpecW,
  const string& searchRandSeed,
  bool doEndGameIfAllPassAlive, bool clearBotBeforeSearch,
  Logger& logger, bool logSearchInfo, bool logMoves,
  int maxMovesPerGame, const std::function<bool()>& shouldStop,
  const WaitableFlag* shouldPause,
  const PlaySettings& playSettings, const OtherGameProperties& otherGameProps,
  Rand& gameRand,
  std::function<NNEvaluator*()> checkForNewNNEval,
  std::function<void(const Board&, const BoardHistory&, Player, Loc, const std::vector<double>&, const std::vector<double>&, const std::vector<double>&, const Search*)> onEachMove
) {
  Search* botB;
  Search* botW;
  if(botSpecB.botIdx == botSpecW.botIdx) {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, &logger, searchRandSeed);
    botW = botB;
  }
  else {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, &logger, searchRandSeed + "@B");
    botW = new Search(botSpecW.baseParams, botSpecW.nnEval, &logger, searchRandSeed + "@W");
  }

  FinishedGameData* gameData = runGame(
    startBoard, pla, startHist, extraBlackAndKomi,
    botSpecB, botSpecW,
    botB, botW,
    doEndGameIfAllPassAlive, clearBotBeforeSearch,
    logger, logSearchInfo, logMoves,
    maxMovesPerGame, shouldStop,
    shouldPause,
    playSettings, otherGameProps,
    gameRand,
    checkForNewNNEval,
    onEachMove
  );

  if(botW != botB)
    delete botW;
  delete botB;

  return gameData;
}

FinishedGameData* Play::runGame(
  const Board& startBoard, Player startPla, const BoardHistory& startHist, ExtraBlackAndKomi extraBlackAndKomi,
  MatchPairer::BotSpec& botSpecB, MatchPairer::BotSpec& botSpecW,
  Search* botB, Search* botW,
  bool doEndGameIfAllPassAlive, bool clearBotBeforeSearch,
  Logger& logger, bool logSearchInfo, bool logMoves,
  int maxMovesPerGame, const std::function<bool()>& shouldStop,
  const WaitableFlag* shouldPause,
  const PlaySettings& playSettings, const OtherGameProperties& otherGameProps,
  Rand& gameRand,
  std::function<NNEvaluator*()> checkForNewNNEval,
  std::function<void(const Board&, const BoardHistory&, Player, Loc, const std::vector<double>&, const std::vector<double>&, const std::vector<double>&, const Search*)> onEachMove
) {
  FinishedGameData* gameData = new FinishedGameData();

  Board board(startBoard);
  BoardHistory hist(startHist);
  Player pla = startPla;
  assert(!(extraBlackAndKomi.makeGameFair && extraBlackAndKomi.makeGameFairForEmptyBoard));
  assert(!(playSettings.forSelfPlay && !clearBotBeforeSearch));

  if(extraBlackAndKomi.makeGameFairForEmptyBoard) {
    Board b(startBoard.x_size,startBoard.y_size);
    Player makeFairPla = P_BLACK;
    if(playSettings.flipKomiProbWhenNoCompensate != 0.0 && gameRand.nextBool(playSettings.flipKomiProbWhenNoCompensate))
      makeFairPla = P_WHITE;
    BoardHistory h(b,makeFairPla,startHist.rules,startHist.encorePhase);
    //Restore baseline on empty hist, adjust empty hist to fair, then apply to real history.
    PlayUtils::setKomiWithoutNoise(extraBlackAndKomi,h);
    PlayUtils::adjustKomiToEven(botB,botW,b,h,makeFairPla,playSettings.compensateKomiVisits,otherGameProps,gameRand);
    extraBlackAndKomi.komiMean = h.rules.komi;
    PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
  }
  if(extraBlackAndKomi.extraBlack > 0 && !hist.isGameFinished) {
    double extraBlackTemperature = playSettings.handicapTemperature;
    assert(extraBlackTemperature > 0.0 && extraBlackTemperature < 10.0);
    PlayUtils::playExtraBlack(botB,extraBlackAndKomi.extraBlack,board,hist,extraBlackTemperature,gameRand);
    assert(hist.moveHistory.size() == 0);
  }
  if(extraBlackAndKomi.makeGameFair) {
    //Restore baseline on hist, adjust hist to fair, then apply it with noise.
    PlayUtils::setKomiWithoutNoise(extraBlackAndKomi,hist);
    PlayUtils::adjustKomiToEven(botB,botW,board,hist,pla,playSettings.compensateKomiVisits,otherGameProps,gameRand);
    extraBlackAndKomi.komiMean = hist.rules.komi;
    PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
  }
  else if((extraBlackAndKomi.extraBlack > 0 || otherGameProps.isFork) &&
          playSettings.fancyKomiVarying &&
          gameRand.nextBool(extraBlackAndKomi.extraBlack > 0 ? 0.5 : 0.25)) {
    double origKomi = hist.rules.komi;
    //Restore baseline on hist, adjust hist to fair, then apply it with noise.
    PlayUtils::setKomiWithoutNoise(extraBlackAndKomi,hist);
    PlayUtils::adjustKomiToEven(botB,botW,board,hist,pla,playSettings.compensateKomiVisits,otherGameProps,gameRand);
    double newKomi = hist.rules.komi;

    //Now, randomize between the old and new komi, with extra noise
    double randKomi = gameRand.nextDouble(min(origKomi,newKomi),max(origKomi,newKomi));
    randKomi += 0.75 * sqrt(board.x_size * board.y_size) * gameRand.nextGaussianTruncated(2.5);
    extraBlackAndKomi.komiMean = (float)randKomi;
    PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
  }
  //Vary komi more when things are completely random to set a better prior for how komi affects evals
  if(playSettings.fancyKomiVarying &&
     botB->nnEvaluator->isNeuralNetLess() &&
     (botW == NULL || botW->nnEvaluator->isNeuralNetLess())) {
    double randKomi = hist.rules.komi + 1.5 * sqrt(board.x_size * board.y_size) * gameRand.nextGaussianTruncated(2.5);
    extraBlackAndKomi.komiMean = (float)randKomi;
    PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
  }

  gameData->bName = botSpecB.botName;
  gameData->wName = botSpecW.botName;
  gameData->bIdx = botSpecB.botIdx;
  gameData->wIdx = botSpecW.botIdx;

  gameData->gameHash.hash0 = gameRand.nextUInt64();
  gameData->gameHash.hash1 = gameRand.nextUInt64();

  gameData->drawEquivalentWinsForWhite = botSpecB.baseParams.drawEquivalentWinsForWhite;
  gameData->playoutDoublingAdvantagePla = otherGameProps.playoutDoublingAdvantagePla;
  gameData->playoutDoublingAdvantage = otherGameProps.playoutDoublingAdvantage;

  gameData->numExtraBlack = extraBlackAndKomi.extraBlack;
  gameData->handicapForSgf = extraBlackAndKomi.extraBlack; //overwritten later
  gameData->mode = FinishedGameData::MODE_NORMAL;
  gameData->beganInEncorePhase = 0;
  gameData->usedInitialPosition = 0;

  if(extraBlackAndKomi.extraBlack > 0)
    gameData->mode = FinishedGameData::MODE_HANDICAP;

  //Might get overwritten next as we also play sgfposes and such with asym mode!
  //So this is just a best efforts to make it more prominent for most of the asymmetric games.
  if(gameData->playoutDoublingAdvantage != 0)
    gameData->mode = FinishedGameData::MODE_ASYM;

  if(otherGameProps.isSgfPos)
    gameData->mode = FinishedGameData::MODE_SGFPOS;
  if(otherGameProps.isHintPos)
    gameData->mode = FinishedGameData::MODE_HINTPOS;

  if(otherGameProps.isHintFork)
    gameData->mode = FinishedGameData::MODE_HINTFORK;
  else if(otherGameProps.isFork)
    gameData->mode = FinishedGameData::MODE_FORK;

  //In selfplay, record all the policy maps and evals and such as well for training data
  bool recordFullData = playSettings.forSelfPlay;

  //NOTE: that checkForNewNNEval might also cause the old nnEval to be invalidated and freed. This is okay since the only
  //references we both hold on to and use are the ones inside the bots here, and we replace the ones in the botSpecs.
  //We should NOT ever store an nnEval separately from these.
  auto maybeCheckForNewNNEval = [&botB,&botW,&botSpecB,&botSpecW,&checkForNewNNEval,&gameRand,&gameData](int nextTurnIdx) {
    //Check if we got a new nnEval, with some probability.
    //Randomized and low-probability so as to reduce contention in checking, while still probably happening in a timely manner.
    if(checkForNewNNEval != nullptr && gameRand.nextBool(0.1)) {
      NNEvaluator* newNNEval = checkForNewNNEval();
      if(newNNEval != NULL) {
        botB->setNNEval(newNNEval);
        if(botW != botB)
          botW->setNNEval(newNNEval);
        botSpecB.nnEval = newNNEval;
        botSpecW.nnEval = newNNEval;
        gameData->changedNeuralNets.push_back(new ChangedNeuralNet(newNNEval->getModelName(),nextTurnIdx));
      }
    }
  };

  if(playSettings.initGamesWithPolicy && otherGameProps.allowPolicyInit && !hist.isGameFinished) {
    double proportionOfBoardArea = otherGameProps.isSgfPos ? playSettings.startPosesPolicyInitAreaProp : playSettings.policyInitAreaProp;
    if(proportionOfBoardArea > 0) {
      //Perform the initialization using a different noised komi, to get a bit of opening policy mixing across komi
      {
        float oldKomi = hist.rules.komi;
        PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
        double policyInitGammaShape = playSettings.policyInitGammaShape;
        double temperature = playSettings.policyInitAreaTemperature;
        assert(temperature > 0.0 && temperature < 10.0);
        PlayUtils::initializeGameUsingPolicy(botB, botW, board, hist, pla, gameRand, doEndGameIfAllPassAlive, proportionOfBoardArea, policyInitGammaShape, temperature);
        hist.setKomi(oldKomi);
      }
      bool shouldCompensate =
        playSettings.compensateAfterPolicyInitProb > 0.0 && gameRand.nextBool(playSettings.compensateAfterPolicyInitProb);
      if(gameData->mode != FinishedGameData::MODE_NORMAL)
        shouldCompensate = extraBlackAndKomi.makeGameFair;
      if(shouldCompensate) {
        PlayUtils::adjustKomiToEven(botB,botW,board,hist,pla,playSettings.compensateKomiVisits,otherGameProps,gameRand);
        extraBlackAndKomi.komiMean = hist.rules.komi;
        PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);
      }
    }
  }

  //Make sure there's some minimum tiny amount of data about how the encore phases work
  if(
    playSettings.forSelfPlay &&
    !otherGameProps.isHintPos &&
    hist.rules.scoringRule == Rules::SCORING_TERRITORY &&
    hist.encorePhase == 0 &&
    gameRand.nextBool(0.04) &&
    !hist.isGameFinished
  ) {
    //Play out to go a quite a bit later in the game.
    double proportionOfBoardArea = 0.25;
    double policyInitGammaShape = 1.0 * 0.8 + playSettings.policyInitGammaShape * 0.2;
    double temperature = 2.0/3.0;
    PlayUtils::initializeGameUsingPolicy(botB, botW, board, hist, pla, gameRand, doEndGameIfAllPassAlive, proportionOfBoardArea, policyInitGammaShape, temperature);

    if(!hist.isGameFinished) {
      //Even out the game
      PlayUtils::adjustKomiToEven(botB, botW, board, hist, pla, playSettings.compensateKomiVisits, otherGameProps, gameRand);
      extraBlackAndKomi.komiMean = hist.rules.komi;
      PlayUtils::setKomiWithNoise(extraBlackAndKomi,hist,gameRand);

      //Randomly set to one of the encore phases
      //Since we played out the game a bunch we should get a good mix of stones that were present or not present at the start
      //of the second encore phase if we're going into the second.
      int encorePhase = gameRand.nextInt(1,2);
      board.clearSimpleKoLoc();
      hist.clear(board,pla,hist.rules,encorePhase);

      gameData->mode = FinishedGameData::MODE_CLEANUP_TRAINING;
      gameData->beganInEncorePhase = encorePhase;
      gameData->usedInitialPosition = 0;
    }
  }

  //Set in the starting board and history to gameData and both bots
  gameData->startBoard = board;
  gameData->startHist = hist;
  gameData->startPla = pla;

  botB->setPosition(pla,board,hist);
  if(botB != botW)
    botW->setPosition(pla,board,hist);

  vector<Loc> locsBuf;
  vector<double> playSelectionValuesBuf;

  vector<SidePosition*> sidePositionsToSearch;

  vector<double> historicalMctsWinLossValues;
  vector<double> historicalMctsLeads;
  vector<double> historicalMctsScoreStdevs;
  vector<ReportedSearchValues> rawNNValues;

  ClockTimer timer;

  //Main play loop
  for(int i = 0; i<maxMovesPerGame; i++) {
    if(doEndGameIfAllPassAlive)
      hist.endGameIfAllPassAlive(board);
    if(hist.isGameFinished)
      break;
    if(shouldPause != nullptr)
      shouldPause->waitUntilFalse();
    if(shouldStop != nullptr && shouldStop())
      break;

    Search* toMoveBot = pla == P_BLACK ? botB : botW;
    if(playSettings.dynamicSelfKomiBonusMin != 0.0 || playSettings.dynamicSelfKomiBonusMax != 0.0) {
      //This might NOT work with selfplay data recording, we'd need to check on stuff like lead and such that
      //it doesn't get fiddled with. For now, use only for match
      if(botB == botW || recordFullData)
        throw StringError("Dynamic komi for matches only right now");
      double plaFactor = pla == P_BLACK ? -1 : 1;
      double currentKomiBonus = plaFactor * (toMoveBot->rootHistory.rules.komi - hist.rules.komi);
      if(historicalMctsWinLossValues.size() >= 2) {
        double prevWinLoss = plaFactor * historicalMctsWinLossValues[historicalMctsWinLossValues.size()-2];
        if(prevWinLoss < playSettings.dynamicSelfKomiWinLossMin)
          currentKomiBonus += 0.5;
        if(prevWinLoss > playSettings.dynamicSelfKomiWinLossMax)
          currentKomiBonus -= 0.5;
      }
      if(currentKomiBonus < playSettings.dynamicSelfKomiBonusMin)
        currentKomiBonus = playSettings.dynamicSelfKomiBonusMin;
      if(currentKomiBonus > playSettings.dynamicSelfKomiBonusMax)
        currentKomiBonus = playSettings.dynamicSelfKomiBonusMax;
      toMoveBot->setKomiIfNew((float)(plaFactor * currentKomiBonus + hist.rules.komi));
    }

    SearchLimitsThisMove limits = getSearchLimitsThisMove(
      toMoveBot, pla, playSettings, gameRand, historicalMctsWinLossValues, clearBotBeforeSearch, otherGameProps
    );
    Loc loc;
    if(playSettings.recordTimePerMove) {
      double t0 = timer.getSeconds();
      loc = runBotWithLimits(toMoveBot, pla, playSettings, limits);
      double t1 = timer.getSeconds();
      if(pla == P_BLACK)
        gameData->bTimeUsed += t1-t0;
      else
        gameData->wTimeUsed += t1-t0;
    }
    else {
      loc = runBotWithLimits(toMoveBot, pla, playSettings, limits);
    }

    if(pla == P_BLACK)
      gameData->bMoveCount += 1;
    else
      gameData->wMoveCount += 1;

    if(loc == Board::NULL_LOC || !toMoveBot->isLegalStrict(loc,pla))
      failIllegalMove(toMoveBot,logger,board,loc);
    if(logSearchInfo)
      logSearch(toMoveBot,logger,loc,otherGameProps);
    if(logMoves)
      logger.write("Move " + Global::uint64ToString(hist.moveHistory.size()) + " made: " + Location::toString(loc,board));

    ValueTargets whiteValueTargets;
    extractValueTargets(whiteValueTargets, toMoveBot, toMoveBot->rootNode);
    gameData->whiteValueTargetsByTurn.push_back(whiteValueTargets);
    QValueTargets whiteQValueTargets;
    extractQValueTargets(whiteQValueTargets.targets, toMoveBot, toMoveBot->rootNode);
    gameData->whiteQValueTargetsByTurn.push_back(whiteQValueTargets);

    if(!recordFullData) {
      //Go ahead and record this anyways with just the visits, as a bit of a hack so that the sgf output can also write the number of visits.
      int64_t unreducedNumVisits = toMoveBot->getRootVisits();
      gameData->policyTargetsByTurn.push_back(PolicyTarget(NULL,unreducedNumVisits));
    }
    else {
      vector<PolicyTargetMove>* policyTarget = new vector<PolicyTargetMove>();
      int64_t unreducedNumVisits = toMoveBot->getRootVisits();
      Play::extractPolicyTarget(*policyTarget, toMoveBot, toMoveBot->rootNode, locsBuf, playSelectionValuesBuf);
      gameData->policyTargetsByTurn.push_back(PolicyTarget(policyTarget,unreducedNumVisits));
      gameData->nnRawStatsByTurn.push_back(computeNNRawStats(toMoveBot, board, hist, pla));

      gameData->targetWeightByTurn.push_back(limits.targetWeight);

      double policySurprise = 0.0, policyEntropy = 0.0, searchEntropy = 0.0;
      bool success = toMoveBot->getPolicySurpriseAndEntropy(policySurprise, searchEntropy, policyEntropy);
      testAssert(success);
      (void)success; //Avoid warning when asserts are disabled
      gameData->policySurpriseByTurn.push_back(policySurprise);
      gameData->policyEntropyByTurn.push_back(policyEntropy);
      gameData->searchEntropyByTurn.push_back(searchEntropy);

      rawNNValues.push_back(toMoveBot->getRootRawNNValuesRequireSuccess());

      //Occasionally fork off some positions to evaluate
      Loc sidePositionForkLoc = Board::NULL_LOC;
      if(playSettings.sidePositionProb > 0.0 && gameRand.nextBool(playSettings.sidePositionProb)) {
        assert(toMoveBot->rootNode != NULL);
        const NNOutput* nnOutput = toMoveBot->rootNode->getNNOutput();
        assert(nnOutput != NULL);
        Loc banMove = loc;
        sidePositionForkLoc = chooseRandomForkingMove(nnOutput, board, hist, pla, gameRand, banMove);
        if(sidePositionForkLoc != Board::NULL_LOC) {
          SidePosition* sp = new SidePosition(board,hist,pla,(int)gameData->changedNeuralNets.size());
          sp->hist.makeBoardMoveAssumeLegal(sp->board,sidePositionForkLoc,sp->pla,NULL);
          sp->pla = getOpp(sp->pla);
          if(sp->hist.isGameFinished) delete sp;
          else sidePositionsToSearch.push_back(sp);
        }
      }

      //If enabled, also record subtree positions from the search as training positions
      if(playSettings.recordTreePositions && playSettings.recordTreeTargetWeight > 0.0f) {
        if(playSettings.recordTreeTargetWeight > 1.0f)
          throw StringError("playSettings.recordTreeTargetWeight > 1.0f");

        recordTreePositions(
          gameData,
          board,hist,pla,
          toMoveBot,
          playSettings.recordTreeThreshold,playSettings.recordTreeTargetWeight,
          (int)gameData->changedNeuralNets.size(),
          locsBuf,playSelectionValuesBuf,
          loc,sidePositionForkLoc
        );
      }
    }

    if(playSettings.allowResignation || playSettings.reduceVisits) {
      ReportedSearchValues values = toMoveBot->getRootValuesRequireSuccess();
      historicalMctsWinLossValues.push_back(values.winLossValue);
      historicalMctsLeads.push_back(values.lead);
      historicalMctsScoreStdevs.push_back(values.expectedScoreStdev);
    }

    if(onEachMove != nullptr)
      onEachMove(board,hist,pla,loc,historicalMctsWinLossValues,historicalMctsLeads,historicalMctsScoreStdevs,toMoveBot);

    //Finally, make the move on the bots
    bool suc;
    suc = botB->makeMove(loc,pla);
    testAssert(suc);
    if(botB != botW) {
      suc = botW->makeMove(loc,pla);
      testAssert(suc);
    }
    (void)suc; //Avoid warning when asserts disabled

    //And make the move on our copy of the board
    testAssert(hist.isLegal(board,loc,pla));
    hist.makeBoardMoveAssumeLegal(board,loc,pla,NULL);

    //Check for resignation
    if(playSettings.allowResignation && historicalMctsWinLossValues.size() >= playSettings.resignConsecTurns) {
      //Play at least some moves no matter what
      int minTurnForResignation = 1 + board.x_size * board.y_size / 5;
      if(i >= minTurnForResignation) {
        if(playSettings.resignThreshold > 0 || std::isnan(playSettings.resignThreshold))
          throw StringError("playSettings.resignThreshold > 0 || std::isnan(playSettings.resignThreshold)");

        bool shouldResign = true;
        for(int j = 0; j<playSettings.resignConsecTurns; j++) {
          double winLossValue = historicalMctsWinLossValues[historicalMctsWinLossValues.size()-j-1];
          Player resignPlayerThisTurn = C_EMPTY;
          if(winLossValue < playSettings.resignThreshold)
            resignPlayerThisTurn = P_WHITE;
          else if(winLossValue > -playSettings.resignThreshold)
            resignPlayerThisTurn = P_BLACK;

          if(resignPlayerThisTurn != pla) {
            shouldResign = false;
            break;
          }
        }

        if(shouldResign)
          hist.setWinnerByResignation(getOpp(pla));
      }
    }

    testAssert(hist.moveHistory.size() < 0x1FFFffff);
    int nextTurnIdx = (int)hist.moveHistory.size();
    maybeCheckForNewNNEval(nextTurnIdx);

    pla = getOpp(pla);
  }

  gameData->endHist = hist;
  if(hist.isGameFinished)
    gameData->hitTurnLimit = false;
  else
    gameData->hitTurnLimit = true;

  // In self-play or match play, it should ALWAYS be the case that the entire game history is legal.
  if(hist.numConsecValidTurnsThisGame != hist.moveHistory.size()) {
    ostringstream sout;
    sout << "Selfplay got history with not entire game legal!?!" << "\n";
    sout << "hist.numConsecValidTurnsThisGame " << hist.numConsecValidTurnsThisGame << "\n";
    sout << "hist.moveHistory.size() " << hist.moveHistory.size() << "\n";
    hist.printBasicInfo(sout,board);
    hist.printDebugInfo(sout,board);
    logger.write(sout.str());
    cerr << sout.str() << endl;
    testAssert(false);
  }

  {
    BoardHistory histCopy(hist);
    //Always use true for computing the handicap value that goes into an sgf
    histCopy.setAssumeMultipleStartingBlackMovesAreHandicap(true);
    gameData->handicapForSgf = histCopy.computeNumHandicapStones();
  }

  if(recordFullData) {
    if(hist.isResignation)
      throw StringError("Recording full data currently incompatible with resignation");

    ValueTargets finalValueTargets;

    assert(gameData->finalFullArea == NULL);
    assert(gameData->finalOwnership == NULL);
    assert(gameData->finalSekiAreas == NULL);
    gameData->finalFullArea = new Color[Board::MAX_ARR_SIZE];
    gameData->finalOwnership = new Color[Board::MAX_ARR_SIZE];
    gameData->finalSekiAreas = new bool[Board::MAX_ARR_SIZE];

    if(hist.isGameFinished && hist.isNoResult) {
      finalValueTargets.win = 0.0f;
      finalValueTargets.loss = 0.0f;
      finalValueTargets.noResult = 1.0f;
      finalValueTargets.score = 0.0f;

      //Fill with empty so that we use "nobody owns anything" as the training target.
      //Although in practice actually the training normally weights by having a result or not, so it doesn't matter what we fill.
      std::fill(gameData->finalFullArea,gameData->finalFullArea+Board::MAX_ARR_SIZE,C_EMPTY);
      std::fill(gameData->finalOwnership,gameData->finalOwnership+Board::MAX_ARR_SIZE,C_EMPTY);
      std::fill(gameData->finalSekiAreas,gameData->finalSekiAreas+Board::MAX_ARR_SIZE,false);
    }
    else {
      //Relying on this to be idempotent, so that we can get the final territory map
      //We also do want to call this here to force-end the game if we crossed a move limit.
      hist.endAndScoreGameNow(board,gameData->finalOwnership);

      finalValueTargets.win = (float)ScoreValue::whiteWinsOfWinner(hist.winner, gameData->drawEquivalentWinsForWhite);
      finalValueTargets.loss = 1.0f - finalValueTargets.win;
      finalValueTargets.noResult = 0.0f;
      finalValueTargets.score = (float)ScoreValue::whiteScoreDrawAdjust(hist.finalWhiteMinusBlackScore,gameData->drawEquivalentWinsForWhite,hist);
      finalValueTargets.hasLead = true;
      finalValueTargets.lead = finalValueTargets.score;

      //Fill full and seki areas
      {
        board.calculateArea(gameData->finalFullArea, true, true, true, hist.rules.multiStoneSuicideLegal);

        Color* independentLifeArea = new Color[Board::MAX_ARR_SIZE];
        int whiteMinusBlackIndependentLifeRegionCount;
        board.calculateIndependentLifeArea(independentLifeArea,whiteMinusBlackIndependentLifeRegionCount, false, false, hist.rules.multiStoneSuicideLegal);
        for(int i = 0; i<Board::MAX_ARR_SIZE; i++) {
          if(independentLifeArea[i] == C_EMPTY && (gameData->finalFullArea[i] == C_BLACK || gameData->finalFullArea[i] == C_WHITE))
            gameData->finalSekiAreas[i] = true;
          else
            gameData->finalSekiAreas[i] = false;
        }
        delete[] independentLifeArea;
      }
    }
    gameData->whiteValueTargetsByTurn.push_back(finalValueTargets);

    //If we had a hintloc, then don't trust the first value, it will be corrupted a bit by the forced playouts.
    //Just copy the next turn's value.
    if(otherGameProps.hintLoc != Board::NULL_LOC) {
      gameData->whiteValueTargetsByTurn[0] = gameData->whiteValueTargetsByTurn[std::min((size_t)1,gameData->whiteValueTargetsByTurn.size()-1)];
    }

    assert(gameData->finalWhiteScoring == NULL);
    gameData->finalWhiteScoring = new float[Board::MAX_ARR_SIZE];
    NNInputs::fillScoring(board,gameData->finalOwnership,hist.rules.taxRule == Rules::TAX_ALL,gameData->finalWhiteScoring);

    gameData->hasFullData = true;

    vector<double> valueSurpriseByTurn;
    {
      const vector<ValueTargets>& whiteValueTargetsByTurn = gameData->whiteValueTargetsByTurn;
      assert(whiteValueTargetsByTurn.size() == gameData->targetWeightByTurn.size() + 1);
      assert(rawNNValues.size() == gameData->targetWeightByTurn.size());
      valueSurpriseByTurn.resize(rawNNValues.size());

      int boardArea = board.x_size * board.y_size;
      double nowFactor = 1.0/(1.0 + boardArea * 0.016);

      double winValue = whiteValueTargetsByTurn[whiteValueTargetsByTurn.size()-1].win;
      double lossValue = whiteValueTargetsByTurn[whiteValueTargetsByTurn.size()-1].loss;
      double noResultValue = whiteValueTargetsByTurn[whiteValueTargetsByTurn.size()-1].noResult;
      for(int i = (int)rawNNValues.size()-1; i >= 0; i--) {
        winValue = winValue + nowFactor * (whiteValueTargetsByTurn[i].win - winValue);
        lossValue = lossValue + nowFactor * (whiteValueTargetsByTurn[i].loss - lossValue);
        noResultValue = noResultValue + nowFactor * (whiteValueTargetsByTurn[i].noResult - noResultValue);

        double valueSurprise = 0.0;
        if(winValue > 1e-100) valueSurprise += winValue * (log(winValue) - log(std::max((double)rawNNValues[i].winValue,1e-100)));
        if(lossValue > 1e-100) valueSurprise += lossValue * (log(lossValue) - log(std::max((double)rawNNValues[i].lossValue,1e-100)));
        if(noResultValue > 1e-100) valueSurprise += noResultValue * (log(noResultValue) - log(std::max((double)rawNNValues[i].noResultValue,1e-100)));

        //Just in case, guard against float imprecision
        if(valueSurprise < 0.0)
          valueSurprise = 0.0;
        //Cap value surprise at extreme value, to reduce the chance of a ridiculous weight on a move.
        valueSurpriseByTurn[i] = std::min(valueSurprise,1.0);
      }
    }

    //Compute desired expectation with which to write main game rows
    if(playSettings.policySurpriseDataWeight > 0 || playSettings.valueSurpriseDataWeight > 0) {
      size_t numWeights = gameData->targetWeightByTurn.size();
      assert(numWeights == gameData->policySurpriseByTurn.size());

      double sumWeights = 0.0;
      double sumPolicySurpriseWeighted = 0.0;
      double sumValueSurpriseWeighted = 0.0;
      for(size_t i = 0; i < numWeights; i++) {
        float targetWeight = gameData->targetWeightByTurn[i];
        assert(targetWeight >= 0.0 && targetWeight <= 1.0);
        sumWeights += targetWeight;
        double policySurprise = gameData->policySurpriseByTurn[i];
        assert(policySurprise >= 0.0);
        double valueSurprise = valueSurpriseByTurn[i];
        assert(valueSurprise >= 0.0);
        sumPolicySurpriseWeighted += policySurprise * targetWeight;
        sumValueSurpriseWeighted += valueSurprise * targetWeight;
      }

      if(sumWeights >= 1) {
        double averagePolicySurpriseWeighted = sumPolicySurpriseWeighted / sumWeights;
        double averageValueSurpriseWeighted = sumValueSurpriseWeighted / sumWeights;

        //It's possible that we have very little value surprise, such as if the game was initialized lopsided and never again changed
        //from that and the expected player won. So if the total value surprise on targetWeighted turns is too small, then also don't
        //do much valueSurpriseDataWeight, since it would be basically dividing by almost zero, in potentially weird ways.
        double valueSurpriseDataWeight = playSettings.valueSurpriseDataWeight;
        if(averageValueSurpriseWeighted < 0.010) { //0.010 logits on average, pretty arbitrary, mainly just intended limit to extreme cases.
          valueSurpriseDataWeight *= averageValueSurpriseWeighted / 0.010;
        }

        //We also include some rows from non-full searches, if despite the shallow search
        //they were quite surprising to the policy.
        double thresholdToIncludeReduced = averagePolicySurpriseWeighted * 1.5;

        //Part of the weight will be proportional to surprisePropValue which is just policySurprise on normal rows
        //and the excess policySurprise beyond threshold on shallow searches.
        //First pass - we sum up the surpriseValue.
        double sumPolicySurprisePropValue = 0.0;
        double sumValueSurprisePropValue = 0.0;
        for(int i = 0; i<numWeights; i++) {
          float targetWeight = gameData->targetWeightByTurn[i];
          double policySurprise = gameData->policySurpriseByTurn[i];
          double valueSurprise = valueSurpriseByTurn[i];
          double policySurprisePropValue =
            targetWeight * policySurprise + (1-targetWeight) * std::max(0.0,policySurprise-thresholdToIncludeReduced);
          double valueSurprisePropValue =
            targetWeight * valueSurprise;
          sumPolicySurprisePropValue += policySurprisePropValue;
          sumValueSurprisePropValue += valueSurprisePropValue;
        }

        //Just in case, avoid div by 0
        sumPolicySurprisePropValue = std::max(sumPolicySurprisePropValue,1e-10);
        sumValueSurprisePropValue = std::max(sumValueSurprisePropValue,1e-10);

        for(int i = 0; i<numWeights; i++) {
          float targetWeight = gameData->targetWeightByTurn[i];
          double policySurprise = gameData->policySurpriseByTurn[i];
          double valueSurprise = valueSurpriseByTurn[i];
          double policySurprisePropValue =
            targetWeight * policySurprise + (1-targetWeight) * std::max(0.0,policySurprise-thresholdToIncludeReduced);
          double valueSurprisePropValue =
            targetWeight * valueSurprise;
          double newValue =
            (1.0-playSettings.policySurpriseDataWeight-valueSurpriseDataWeight) * targetWeight
            + playSettings.policySurpriseDataWeight * policySurprisePropValue * sumWeights / sumPolicySurprisePropValue
            + valueSurpriseDataWeight * valueSurprisePropValue * sumWeights / sumValueSurprisePropValue;
          gameData->targetWeightByTurn[i] = (float)(newValue);
        }
      }
    }

    //Also evaluate all the side positions as well that we queued up to be searched
    NNResultBuf nnResultBuf;
    for(int i = 0; i<sidePositionsToSearch.size(); i++) {
      SidePosition* sp = sidePositionsToSearch[i];

      if(shouldPause != nullptr)
        shouldPause->waitUntilFalse();
      if(shouldStop != nullptr && shouldStop()) {
        delete sp;
        continue;
      }

      Search* toMoveBot = sp->pla == P_BLACK ? botB : botW;
      toMoveBot->setPosition(sp->pla,sp->board,sp->hist);
      //We do NOT apply playoutDoublingAdvantage here. If changing this, note that it is coordinated with train data writing
      //not using playoutDoublingAdvantage for these rows too.
      assert(toMoveBot->searchParams.playoutDoublingAdvantage == 0.0);
      assert(toMoveBot->searchParams.playoutDoublingAdvantagePla == C_EMPTY);
      sp->playoutDoublingAdvantagePla = C_EMPTY;
      sp->playoutDoublingAdvantage = 0.0;
      Loc responseLoc = toMoveBot->runWholeSearchAndGetMove(sp->pla);

      Play::extractPolicyTarget(sp->policyTarget, toMoveBot, toMoveBot->rootNode, locsBuf, playSelectionValuesBuf);
      extractValueTargets(sp->whiteValueTargets, toMoveBot, toMoveBot->rootNode);
      extractQValueTargets(sp->whiteQValueTargets.targets, toMoveBot, toMoveBot->rootNode);

      double policySurprise = 0.0, policyEntropy = 0.0, searchEntropy = 0.0;
      bool success = toMoveBot->getPolicySurpriseAndEntropy(policySurprise, searchEntropy, policyEntropy);
      testAssert(success);
      (void)success; //Avoid warning when asserts are disabled
      sp->policySurprise = policySurprise;
      sp->policyEntropy = policyEntropy;
      sp->searchEntropy = searchEntropy;

      sp->nnRawStats = computeNNRawStats(toMoveBot, sp->board, sp->hist, sp->pla);
      sp->targetWeight = 1.0f;
      sp->unreducedNumVisits = toMoveBot->getRootVisits();
      sp->numNeuralNetChangesSoFar = (int)gameData->changedNeuralNets.size();

      gameData->sidePositions.push_back(sp);

      //If enabled, also record subtree positions from the search as training positions
      if(playSettings.recordTreePositions && playSettings.recordTreeTargetWeight > 0.0f) {
        if(playSettings.recordTreeTargetWeight > 1.0f)
          throw StringError("playSettings.recordTreeTargetWeight > 1.0f");
        recordTreePositions(
          gameData,
          sp->board,sp->hist,sp->pla,
          toMoveBot,
          playSettings.recordTreeThreshold,playSettings.recordTreeTargetWeight,
          (int)gameData->changedNeuralNets.size(),
          locsBuf,playSelectionValuesBuf,
          Board::NULL_LOC, Board::NULL_LOC
        );
      }

      //Occasionally continue the fork a second move or more, to provide some situations where the opponent has played "weird" moves not
      //only on the most immediate turn, but rather the turns before.
      if(gameRand.nextBool(0.25)) {
        if(responseLoc == Board::NULL_LOC || !sp->hist.isLegal(sp->board,responseLoc,sp->pla))
          failIllegalMove(toMoveBot,logger,sp->board,responseLoc);

        SidePosition* sp2 = new SidePosition(sp->board,sp->hist,sp->pla,(int)gameData->changedNeuralNets.size());
        sp2->hist.makeBoardMoveAssumeLegal(sp2->board,responseLoc,sp2->pla,NULL);
        sp2->pla = getOpp(sp2->pla);
        if(sp2->hist.isGameFinished)
          delete sp2;
        else {
          Search* toMoveBot2 = sp2->pla == P_BLACK ? botB : botW;
          MiscNNInputParams nnInputParams;
          nnInputParams.drawEquivalentWinsForWhite = toMoveBot2->searchParams.drawEquivalentWinsForWhite;
          toMoveBot2->nnEvaluator->evaluate(
            sp2->board,sp2->hist,sp2->pla,nnInputParams,
            nnResultBuf,false,false
          );
          Loc banMove = Board::NULL_LOC;
          Loc forkLoc = chooseRandomForkingMove(nnResultBuf.result.get(), sp2->board, sp2->hist, sp2->pla, gameRand, banMove);
          if(forkLoc != Board::NULL_LOC) {
            sp2->hist.makeBoardMoveAssumeLegal(sp2->board,forkLoc,sp2->pla,NULL);
            sp2->pla = getOpp(sp2->pla);
            if(sp2->hist.isGameFinished) delete sp2;
            else sidePositionsToSearch.push_back(sp2);
          }
        }
      }

      testAssert(gameData->endHist.moveHistory.size() < 0x1FFFffff);
      maybeCheckForNewNNEval((int)gameData->endHist.moveHistory.size());
    }

    if(playSettings.scaleDataWeight != 1.0) {
      for(int i = 0; i<gameData->targetWeightByTurn.size(); i++)
        gameData->targetWeightByTurn[i] = (float)(playSettings.scaleDataWeight * gameData->targetWeightByTurn[i]);
      for(int i = 0; i<gameData->sidePositions.size(); i++)
        gameData->sidePositions[i]->targetWeight = (float)(playSettings.scaleDataWeight * gameData->sidePositions[i]->targetWeight);
    }

    //Record weights before we possibly probabilistically resolve them
    {
      gameData->targetWeightByTurnUnrounded.resize(gameData->targetWeightByTurn.size());
      for(int i = 0; i<gameData->targetWeightByTurn.size(); i++)
        gameData->targetWeightByTurnUnrounded[i] = gameData->targetWeightByTurn[i];
      for(int i = 0; i<gameData->sidePositions.size(); i++)
        gameData->sidePositions[i]->targetWeightUnrounded = gameData->sidePositions[i]->targetWeight;
    }

    //Resolve probabilistic weights of things
    //Do this right now so that if something isn't included at all, we can skip some work, like lead estmation.
    if(!playSettings.noResolveTargetWeights) {
      auto resolveWeight = [&gameRand](float weight){
        if(weight <= 0) weight = 0;
        float floored = floor(weight);
        float excess = weight - floored;
        weight = gameRand.nextBool(excess) ? floored+1 : floored;
        return weight;
      };

      for(int i = 0; i<gameData->targetWeightByTurn.size(); i++)
        gameData->targetWeightByTurn[i] = resolveWeight(gameData->targetWeightByTurn[i]);
      for(int i = 0; i<gameData->sidePositions.size(); i++)
        gameData->sidePositions[i]->targetWeight = resolveWeight(gameData->sidePositions[i]->targetWeight);
    }


    //Fill in lead estimation on full-search positions
    if(playSettings.estimateLeadProb > 0.0) {
      assert(gameData->targetWeightByTurn.size() + 1 == gameData->whiteValueTargetsByTurn.size());
      board = gameData->startBoard;
      hist = gameData->startHist;
      pla = gameData->startPla;

      testAssert(gameData->startHist.moveHistory.size() < 0x1FFFffff);
      testAssert(gameData->endHist.moveHistory.size() < 0x1FFFffff);
      testAssert(gameData->endHist.moveHistory.size() >= gameData->startHist.moveHistory.size());
      int startTurnIdx = (int)gameData->startHist.moveHistory.size();
      int numMoves = (int)(gameData->endHist.moveHistory.size() - gameData->startHist.moveHistory.size());
      for(int turnAfterStart = 0; turnAfterStart<numMoves; turnAfterStart++) {
        int turnIdx = turnAfterStart + startTurnIdx;
        if(gameData->targetWeightByTurn[turnAfterStart] > 0 &&
           //Avoid computing lead when no result was considered to be very likely, since in such cases
           //the relationship between komi and the result can somewhat break.
           gameData->whiteValueTargetsByTurn[turnAfterStart].noResult < 0.3 &&
           gameRand.nextBool(playSettings.estimateLeadProb) &&
           //Or if the actual game ended in no result
           !(gameData->endHist.isGameFinished && gameData->endHist.isNoResult)
        ) {
          if(shouldPause != nullptr)
            shouldPause->waitUntilFalse();
          if(shouldStop != nullptr && shouldStop())
            break;

          gameData->whiteValueTargetsByTurn[turnAfterStart].lead =
            PlayUtils::computeLead(botB,botW,board,hist,pla,playSettings.estimateLeadVisits,otherGameProps);
          gameData->whiteValueTargetsByTurn[turnAfterStart].hasLead = true;
        }
        Move move = gameData->endHist.moveHistory[turnIdx];
        assert(move.pla == pla);
        hist.makeBoardMoveAssumeLegal(board, move.loc, move.pla, NULL);
        pla = getOpp(pla);
      }

      for(int i = 0; i<gameData->sidePositions.size(); i++) {
        SidePosition* sp = gameData->sidePositions[i];
        if(sp->targetWeight > 0 &&
           //Avoid computing lead when no result was considered to be very likely, since in such cases
           //the relationship between komi and the result can somewhat break.
           sp->whiteValueTargets.noResult < 0.3 &&
           gameRand.nextBool(playSettings.estimateLeadProb) &&
           //Or if the non-side-position actual game ended in no result
           !(gameData->endHist.isGameFinished && gameData->endHist.isNoResult)
        ) {
          if(shouldPause != nullptr)
            shouldPause->waitUntilFalse();
          if(shouldStop != nullptr && shouldStop())
            break;

          sp->whiteValueTargets.lead =
            PlayUtils::computeLead(botB,botW,sp->board,sp->hist,sp->pla,playSettings.estimateLeadVisits,otherGameProps);
          sp->whiteValueTargets.hasLead = true;
        }
      }
    }
  }

  gameData->trainingWeight = otherGameProps.trainingWeight;
  return gameData;
}

static void replayGameUpToMove(const FinishedGameData* finishedGameData, int moveIdx, Rules rules, Board& board, BoardHistory& hist, Player& pla) {
  board = finishedGameData->startHist.initialBoard;
  pla = finishedGameData->startHist.initialPla;

  if(rules.scoringRule == Rules::SCORING_AREA)
    hist.clear(board,pla,rules,0);
  else
    hist.clear(board,pla,rules,finishedGameData->startHist.initialEncorePhase);

  //Make sure it's prior to the last move
  if(finishedGameData->endHist.moveHistory.size() <= 0)
    return;
  moveIdx = std::min(moveIdx,(int)(finishedGameData->endHist.moveHistory.size()-1));

  //Replay all those moves
  for(int i = 0; i<moveIdx; i++) {
    Loc loc = finishedGameData->endHist.moveHistory[i].loc;
    if(!hist.isLegal(board,loc,pla)) {
      //We have a bug of some sort if we got an illegal move on replay, unless
      //we are in encore phase (pass for ko may change) or the rules are different
      if(rules == finishedGameData->startHist.rules && hist.encorePhase == 0) {
        cout << board << endl;
        cout << PlayerIO::colorToChar(pla) << endl;
        cout << Location::toString(loc,board) << endl;
        hist.printDebugInfo(cout,board);
        cout << endl;
        throw StringError("Illegal move when replaying to fork game?");
      }
      //Just break out due to the illegal move and stop the replay here
      return;
    }
    assert(finishedGameData->endHist.moveHistory[i].pla == pla);
    hist.makeBoardMoveAssumeLegal(board,loc,pla,NULL);
    pla = getOpp(pla);

    if(hist.isGameFinished)
      return;
  }
}

static bool hasUnownedSpot(const FinishedGameData* finishedGameData) {
  assert(finishedGameData->finalOwnership != NULL);
  const Board& board = finishedGameData->startBoard;
  for(int y = 0; y<board.y_size; y++) {
    for(int x = 0; x<board.x_size; x++) {
      Loc loc = Location::getLoc(x,y,board.x_size);
      if(finishedGameData->finalOwnership[loc] == C_EMPTY)
        return true;
    }
  }
  return false;
}

void Play::maybeForkGame(
  const FinishedGameData* finishedGameData,
  ForkData* forkData,
  const PlaySettings& playSettings,
  Rand& gameRand,
  Search* bot
) {
  if(forkData == NULL)
    return;
  assert(finishedGameData->startHist.initialBoard.pos_hash == finishedGameData->endHist.initialBoard.pos_hash);
  assert(finishedGameData->startHist.initialPla == finishedGameData->endHist.initialPla);

  //Just for conceptual simplicity, don't early fork games that started in the encore
  if(finishedGameData->startHist.encorePhase != 0)
    return;
  bool earlyFork = gameRand.nextBool(playSettings.earlyForkGameProb);
  bool lateFork = !earlyFork && playSettings.forkGameProb > 0 ? gameRand.nextBool(playSettings.forkGameProb) : false;
  if(!earlyFork && !lateFork)
    return;

  //Pick a random move to fork from near the start
  int moveIdx;
  if(earlyFork) {
    moveIdx = (int)floor(
      gameRand.nextExponential() * (
        playSettings.earlyForkGameExpectedMoveProp * finishedGameData->startBoard.x_size * finishedGameData->startBoard.y_size
      )
    );
  }
  else if(lateFork) {
    moveIdx = finishedGameData->endHist.moveHistory.size() <= 0 ? 0 : (int)gameRand.nextUInt((uint32_t)finishedGameData->endHist.moveHistory.size());
  }
  else {
    ASSERT_UNREACHABLE;
  }

  Board board;
  Player pla;
  BoardHistory hist;
  replayGameUpToMove(finishedGameData, moveIdx, finishedGameData->startHist.rules, board, hist, pla);
  //Just in case if somehow the game is over now, don't actually do anything
  if(hist.isGameFinished)
    return;

  //Pick a move!
  if(playSettings.forkGameMaxChoices > NNPos::MAX_NN_POLICY_SIZE)
    throw StringError("playSettings.forkGameMaxChoices > NNPos::MAX_NN_POLICY_SIZE");
  if(playSettings.earlyForkGameMaxChoices > NNPos::MAX_NN_POLICY_SIZE)
    throw StringError("playSettings.earlyForkGameMaxChoices > NNPos::MAX_NN_POLICY_SIZE");
  int maxChoices = earlyFork ? playSettings.earlyForkGameMaxChoices : playSettings.forkGameMaxChoices;
  if(maxChoices < playSettings.forkGameMinChoices)
    throw StringError("playSettings fork game max choices < playSettings.forkGameMinChoices");

  //Generate a selection of a small random number of choices
  int numChoices = gameRand.nextInt(playSettings.forkGameMinChoices, maxChoices);
  assert(numChoices <= NNPos::MAX_NN_POLICY_SIZE);
  Loc possibleMoves[NNPos::MAX_NN_POLICY_SIZE];
  int numPossible = PlayUtils::chooseRandomLegalMoves(board,hist,pla,gameRand,possibleMoves,numChoices);
  if(numPossible <= 0)
    return;

  //Try the one the value net thinks is best
  Loc bestMove = Board::NULL_LOC;
  double bestScore = 0.0;

  NNResultBuf buf;
  double drawEquivalentWinsForWhite = 0.5;
  for(int i = 0; i<numChoices; i++) {
    Loc loc = possibleMoves[i];
    Board copy = board;
    BoardHistory copyHist = hist;
    copyHist.makeBoardMoveAssumeLegal(copy,loc,pla,NULL);
    MiscNNInputParams nnInputParams;
    nnInputParams.drawEquivalentWinsForWhite = drawEquivalentWinsForWhite;
    bot->nnEvaluator->evaluate(copy,copyHist,getOpp(pla),nnInputParams,buf,false,false);
    std::shared_ptr<NNOutput> nnOutput = std::move(buf.result);
    double whiteScore = nnOutput->whiteScoreMean;
    if(bestMove == Board::NULL_LOC || (pla == P_WHITE && whiteScore > bestScore) || (pla == P_BLACK && whiteScore < bestScore)) {
      bestMove = loc;
      bestScore = whiteScore;
    }
  }

  //Make that move
  testAssert(hist.isLegal(board,bestMove,pla));
  hist.makeBoardMoveAssumeLegal(board,bestMove,pla,NULL);
  pla = getOpp(pla);

  //If the game is over now, don't actually do anything
  if(hist.isGameFinished)
    return;
  forkData->add(new InitialPosition(board,hist,pla,true,false,false,finishedGameData->trainingWeight));
}


void Play::maybeSekiForkGame(
  const FinishedGameData* finishedGameData,
  ForkData* forkData,
  const PlaySettings& playSettings,
  const GameInitializer* gameInit,
  Rand& gameRand
) {
  if(forkData == NULL)
    return;
  if(playSettings.sekiForkHackProb <= 0)
    return;

  //If there are any unowned spots, consider forking the last bit of the game, with random rules and even score
  //Don't fork games starting in second encore though
  const BoardHistory& endHist = finishedGameData->endHist;
  if(endHist.isGameFinished && endHist.isScored && finishedGameData->startHist.encorePhase < 2 && hasUnownedSpot(finishedGameData)) {

    for(int i = 0; i<2; i++) {
      //Pick a random move to fork from near the end of the game
      int moveIdx = (int)floor(endHist.moveHistory.size() * (1.0 - 0.10 * gameRand.nextExponential()) - 1.0);
      if(moveIdx < 0)
        moveIdx = 0;
      if(moveIdx > endHist.moveHistory.size())
        moveIdx = (int)endHist.moveHistory.size();

      //Randomly permute the rules
      Rules rules = finishedGameData->startHist.rules;
      rules = gameInit->randomizeScoringAndTaxRules(rules,gameRand);

      Board board;
      Player pla;
      BoardHistory hist;
      replayGameUpToMove(finishedGameData, moveIdx, rules, board, hist, pla);
      //Just in case if somehow the game is over now, don't actually do anything
      if(hist.isGameFinished)
        continue;
      forkData->addSeki(new InitialPosition(board,hist,pla,false,true,false,finishedGameData->trainingWeight),gameRand);
    }
  }
}

void Play::maybeHintForkGame(
  const FinishedGameData* finishedGameData,
  ForkData* forkData,
  const OtherGameProperties& otherGameProps
) {
  if(forkData == NULL)
    return;
  //Just for conceptual simplicity, don't early fork games that started in the encore
  if(finishedGameData->startHist.encorePhase != 0)
    return;
  bool hintFork =
    otherGameProps.hintLoc != Board::NULL_LOC &&
    finishedGameData->startBoard.pos_hash == otherGameProps.hintPosHash &&
    finishedGameData->startHist.moveHistory.size() == otherGameProps.hintTurn &&
    finishedGameData->endHist.moveHistory.size() > finishedGameData->startHist.moveHistory.size() &&
    finishedGameData->endHist.moveHistory[finishedGameData->startHist.moveHistory.size()].loc != otherGameProps.hintLoc;

  if(!hintFork)
    return;

  Board board;
  Player pla;
  BoardHistory hist;
  testAssert(finishedGameData->startHist.moveHistory.size() < 0x1FFFffff);
  int moveIdxToReplayTo = (int)finishedGameData->startHist.moveHistory.size();
  replayGameUpToMove(finishedGameData, moveIdxToReplayTo, finishedGameData->startHist.rules, board, hist, pla);
  //Just in case if somehow the game is over now, don't actually do anything
  if(hist.isGameFinished)
    return;

  testAssert(pla == hist.presumedNextMovePla);
  if(!hist.isLegal(board,otherGameProps.hintLoc,pla))
    return;

  hist.makeBoardMoveAssumeLegal(board,otherGameProps.hintLoc,pla,NULL);
  pla = getOpp(pla);

  //If the game is over now, don't actually do anything
  if(hist.isGameFinished)
    return;
  forkData->add(new InitialPosition(board,hist,pla,false,false,true,finishedGameData->trainingWeight));
}


GameRunner::GameRunner(ConfigParser& cfg, PlaySettings pSettings, Logger& logger)
  :logSearchInfo(),logMoves(),maxMovesPerGame(),clearBotBeforeSearch(),
   playSettings(pSettings),
   gameInit(NULL)
{
  logSearchInfo = cfg.getBool("logSearchInfo");
  logMoves = cfg.getBool("logMoves");
  maxMovesPerGame = cfg.getInt("maxMovesPerGame",0,1 << 30);
  clearBotBeforeSearch = cfg.contains("clearBotBeforeSearch") ? cfg.getBool("clearBotBeforeSearch") : false;

  //Initialize object for randomizing game settings
  gameInit = new GameInitializer(cfg,logger);
}
GameRunner::GameRunner(ConfigParser& cfg, const string& gameInitRandSeed, PlaySettings pSettings, Logger& logger)
  :logSearchInfo(),logMoves(),maxMovesPerGame(),clearBotBeforeSearch(),
   playSettings(pSettings),
   gameInit(NULL)
{
  logSearchInfo = cfg.getBool("logSearchInfo");
  logMoves = cfg.getBool("logMoves");
  maxMovesPerGame = cfg.getInt("maxMovesPerGame",0,1 << 30);
  clearBotBeforeSearch = cfg.contains("clearBotBeforeSearch") ? cfg.getBool("clearBotBeforeSearch") : false;

  //Initialize object for randomizing game settings
  gameInit = new GameInitializer(cfg,logger,gameInitRandSeed);
}

GameRunner::~GameRunner() {
  delete gameInit;
}

const GameInitializer* GameRunner::getGameInitializer() const {
  return gameInit;
}

FinishedGameData* GameRunner::runGame(
  const string& seed,
  const MatchPairer::BotSpec& bSpecB,
  const MatchPairer::BotSpec& bSpecW,
  ForkData* forkData,
  const Sgf::PositionSample* startPosSample,
  Logger& logger,
  const std::function<bool()>& shouldStop,
  const WaitableFlag* shouldPause,
  std::function<NNEvaluator*()> checkForNewNNEval,
  std::function<void(const MatchPairer::BotSpec&, Search*)> afterInitialization,
  std::function<void(const Board&, const BoardHistory&, Player, Loc, const std::vector<double>&, const std::vector<double>&, const std::vector<double>&, const Search*)> onEachMove
) {
  MatchPairer::BotSpec botSpecB = bSpecB;
  MatchPairer::BotSpec botSpecW = bSpecW;

  Rand gameRand(seed + ":" + "forGameRand");

  const InitialPosition* initialPosition = NULL;
  bool usedSekiForkHackPosition = false;
  if(forkData != NULL) {
    initialPosition = forkData->get(gameRand);

    if(initialPosition == NULL && playSettings.sekiForkHackProb > 0 && gameRand.nextBool(playSettings.sekiForkHackProb)) {
      initialPosition = forkData->getSeki(gameRand);
      if(initialPosition != NULL)
        usedSekiForkHackPosition = true;
    }
  }

  Board board;
  Player pla;
  BoardHistory hist;
  ExtraBlackAndKomi extraBlackAndKomi;
  OtherGameProperties otherGameProps;
  if(playSettings.forSelfPlay) {
    assert(botSpecB.botIdx == botSpecW.botIdx);
    SearchParams params = botSpecB.baseParams;
    gameInit->createGame(board,pla,hist,extraBlackAndKomi,params,initialPosition,playSettings,otherGameProps,startPosSample);
    botSpecB.baseParams = params;
    botSpecW.baseParams = params;
  }
  else {
    gameInit->createGame(board,pla,hist,extraBlackAndKomi,initialPosition,playSettings,otherGameProps,startPosSample);

    bool rulesWereSupported;
    if(botSpecB.nnEval != NULL) {
      botSpecB.nnEval->getSupportedRules(hist.rules,rulesWereSupported);
      if(!rulesWereSupported)
        logger.write("WARNING: Match is running bot on rules that it does not support: " + botSpecB.botName);
    }
    if(botSpecW.nnEval != NULL) {
      botSpecW.nnEval->getSupportedRules(hist.rules,rulesWereSupported);
      if(!rulesWereSupported)
        logger.write("WARNING: Match is running bot on rules that it does not support: " + botSpecW.botName);
    }
  }

  bool clearBotBeforeSearchThisGame = clearBotBeforeSearch;
  if(botSpecB.botIdx == botSpecW.botIdx) {
    //Avoid interactions between the two bots since they're the same.
    //Also in self-play this makes sure root noise is effective on each new search
    clearBotBeforeSearchThisGame = true;
  }

  //In 2% of games, don't autoterminate the game upon all pass alive, to just provide a tiny bit of training data on positions that occur
  //as both players must wrap things up manually, because within the search we don't autoterminate games, meaning that the NN will get
  //called on positions that occur after the game would have been autoterminated.
  bool doEndGameIfAllPassAlive = playSettings.forSelfPlay ? gameRand.nextBool(0.98) : true;

  Search* botB;
  Search* botW;
  if(botSpecB.botIdx == botSpecW.botIdx) {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, &logger, seed);
    botW = botB;
  }
  else {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, &logger, seed + "@B");
    botW = new Search(botSpecW.baseParams, botSpecW.nnEval, &logger, seed + "@W");
  }
  if(afterInitialization != nullptr) {
    if(botSpecB.botIdx == botSpecW.botIdx) {
      afterInitialization(botSpecB,botB);
    }
    else {
      afterInitialization(botSpecB,botB);
      afterInitialization(botSpecW,botW);
    }
  }

  FinishedGameData* finishedGameData = Play::runGame(
    board,pla,hist,extraBlackAndKomi,
    botSpecB,botSpecW,
    botB,botW,
    doEndGameIfAllPassAlive,clearBotBeforeSearchThisGame,
    logger,logSearchInfo,logMoves,
    maxMovesPerGame,shouldStop,
    shouldPause,
    playSettings,otherGameProps,
    gameRand,
    checkForNewNNEval, //Note that if this triggers, botSpecB and botSpecW will get updated, for use in maybeForkGame
    onEachMove
  );

  if(initialPosition != NULL) {
    finishedGameData->usedInitialPosition = 1;
    testAssert(finishedGameData->trainingWeight == initialPosition->trainingWeight);
  }
  else if(startPosSample != NULL) {
    testAssert(finishedGameData->trainingWeight == startPosSample->trainingWeight);
  }

  assert(finishedGameData->trainingWeight > 0.0);
  assert(finishedGameData->trainingWeight < 5.0);

  //Make sure not to write the game if we terminated in the middle of this game!
  if(shouldStop != nullptr && shouldStop()) {
    if(botW != botB)
      delete botW;
    delete botB;
    delete finishedGameData;
    return NULL;
  }

  assert(finishedGameData != NULL);

  Play::maybeForkGame(finishedGameData, forkData, playSettings, gameRand, botB);
  if(!usedSekiForkHackPosition) {
    Play::maybeSekiForkGame(finishedGameData, forkData, playSettings, gameInit, gameRand);
  }
  Play::maybeHintForkGame(finishedGameData, forkData, otherGameProps);

  if(botW != botB)
    delete botW;
  delete botB;

  if(initialPosition != NULL)
    delete initialPosition;

  return finishedGameData;
}
