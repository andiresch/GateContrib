/*----------------------
  GATE version name: gate_v6

  Copyright (C): OpenGATE Collaboration

  This software is distributed under the terms
  of the GNU Lesser General  Public Licence (LGPL)
  See GATE/LICENSE.txt for further details
  ----------------------*/

#include "GateConfiguration.h"
#ifdef GATE_USE_RTK

// Gate
#include "GateHybridForcedDetectionActor.hh"
#include "GateMiscFunctions.hh"
#include "GateScatterOrderTrackInformationActor.hh"
#include "GateHybridForcedDetectionFunctors.hh"

// G4
#include <G4Event.hh>
#include <G4MaterialTable.hh>
#include <G4ParticleTable.hh>
#include <G4VEmProcess.hh>
#include <G4TransportationManager.hh>
#include <G4LivermoreComptonModel.hh>
#include <G4SteppingManager.hh>
#include <G4NistManager.hh>

// rtk
#include <rtkThreeDCircularProjectionGeometryXMLFile.h>
#include <rtkMacro.h>

// itk
#include <itkImportImageFilter.h>
#include <itkChangeInformationImageFilter.h>
#include <itkMultiplyImageFilter.h>
#include <itkConstantPadImageFilter.h>
#include <itkImageFileWriter.h>
#include <itkBinaryFunctorImageFilter.h>
#include <itkAddImageFilter.h>
#include <itksys/SystemTools.hxx>

//-----------------------------------------------------------------------------
/// Constructors
GateHybridForcedDetectionActor::GateHybridForcedDetectionActor(G4String name, G4int depth):
  GateVActor(name,depth),
  mIsSecondarySquaredImageEnabled(false),
  mIsSecondaryUncertaintyImageEnabled(false),
  mWaterLUTMaterial("G4_WATER"),
  mNoisePrimary(0),
  mInputRTKGeometryFilename(""),
  mRussianRouletteSpacing(20.),
  mRussianRouletteMinimumCountInRegion(10),
  mRussianRouletteMinimumProbability(0.0001),
  mSecondPassPhaseSpace(NULL)
{
  GateDebugMessageInc("Actor",4,"GateHybridForcedDetectionActor() -- begin"<<G4endl);
  pActorMessenger = new GateHybridForcedDetectionActorMessenger(this);
  mDetectorResolution[0] = mDetectorResolution[1] = mDetectorResolution[2] = 1;
  GateDebugMessageDec("Actor",4,"GateHybridForcedDetectionActor() -- end"<<G4endl);

  mMapProcessNameWithType["Compton"] = COMPTON;
  mMapProcessNameWithType["compt"]   = COMPTON;
  mMapProcessNameWithType["RayleighScattering"] = RAYLEIGH;
  mMapProcessNameWithType["Rayl"]               = RAYLEIGH;
  mMapProcessNameWithType["PhotoElectric"] = PHOTOELECTRIC;
  mMapProcessNameWithType["phot"]          = PHOTOELECTRIC;

  mMapTypeWithProcessName[COMPTON] = "Compton";
  mMapTypeWithProcessName[RAYLEIGH] = "Rayleigh";
  mMapTypeWithProcessName[PHOTOELECTRIC] = "PhotoElectric";
}
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
/// Destructor
GateHybridForcedDetectionActor::~GateHybridForcedDetectionActor()
{
  delete pActorMessenger;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
/// Construct
void GateHybridForcedDetectionActor::Construct()
{
  GateVActor::Construct();
  //  Callbacks
  EnableBeginOfRunAction(true);
  EnableEndOfRunAction(true);
  EnableBeginOfEventAction(true);
  EnableEndOfEventAction(true);
  //   EnablePreUserTrackingAction(true);
  EnableUserSteppingAction(true);
  ResetData();
  mEMCalculator = new G4EmCalculator;

  CreatePhaseSpace(mPhaseSpaceFilename, mPhaseSpaceFile, mPhaseSpace);
  if(mSecondPassPrefix != "")
    CreatePhaseSpace(AddPrefix(mSecondPassPrefix, mPhaseSpaceFilename),
                     mSecondPassPhaseSpaceFile,
                     mSecondPassPhaseSpace);
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Callback Begin of Run
void GateHybridForcedDetectionActor::BeginOfRunAction(const G4Run*r)
{
  GateVActor::BeginOfRunAction(r);
  mNumberOfEventsInRun = 0;

  // Get information on the source
  GateSourceMgr * sm = GateSourceMgr::GetInstance();
  if (sm->GetNumberOfSources() == 0) {
    GateError("No source set. Abort.");
  }
  if (sm->GetNumberOfSources() != 1) {
    GateWarning("Several sources found, we consider the first one.");
  }
  mSource = sm->GetSource(0);

  // Checks. FIXME: check on rot1 and rot2 would be required
  if(mSource->GetPosDist()->GetPosDisType() == "Point") {
    if(mSource->GetAngDist()->GetDistType() != "iso") {
      GateError("Forced detection only supports iso distributions with Point source.");
    }
  }
  else if(mSource->GetPosDist()->GetPosDisType() == "Plane") {
    if(mSource->GetAngDist()->GetDistType() != "focused") {
      GateError("Forced detection only supports focused distributions for Plane sources.");
    }
  }
  else
    GateError("Forced detection only supports Point and Plane distributions.");

  // Read the response detector curve from an external file
  mEnergyResponseDetector.ReadResponseDetectorFile(mResponseFilename);

  // Create list of energies
  double energyMax = 0.;
  std::vector<double> energyList;
  std::vector<double> energyWeightList;
  G4String st = mSource->GetEneDist()->GetEnergyDisType();
  if (st == "Mono") {
    energyList.push_back(mSource->GetEneDist()->GetMonoEnergy());
    energyWeightList.push_back(mEnergyResponseDetector(energyList.back()));
    energyMax = std::max(energyMax, energyList.back());
  }
  else if (st == "User") { // histo
    G4PhysicsOrderedFreeVector h = mSource->GetEneDist()->GetUserDefinedEnergyHisto ();
    double weightSum = 0.;
    for(unsigned int i=0; i<h.GetVectorLength(); i++) {
      double E = h.Energy(i);
      energyList.push_back(E);
      energyWeightList.push_back(h.Value(E));
      weightSum += energyWeightList.back();
      //noise is desactivated
      if(mNoisePrimary == 0) {
	energyWeightList.back() *= mEnergyResponseDetector(energyList.back());
      }
      energyMax = std::max(energyMax, energyList.back());
    }
    for(unsigned int i=0; i<h.GetVectorLength(); i++)
      energyWeightList[i] /= weightSum;
  }
  else
    GateError("Error, source type is not Mono or User. Abort.");


  // Search for voxelized volume. If more than one, crash (yet).
  GateVImageVolume* gate_image_volume = NULL;
  for(std::map<G4String, GateVVolume*>::const_iterator it  = GateObjectStore::GetInstance()->begin();
                                                       it != GateObjectStore::GetInstance()->end();
                                                       it++)
    {
    if(dynamic_cast<GateVImageVolume*>(it->second))
    {
      if(gate_image_volume != NULL)
        GateError("There is more than one voxelized volume and don't know yet how to cope with this.");
      else
        gate_image_volume = dynamic_cast<GateVImageVolume*>(it->second);
    }
  }
  if(!gate_image_volume)
    GateError("You need a voxelized volume in your scene.");

  // TODO: loop on volumes to check that they contain world material only

  // Conversion of CT to ITK and to int values
  mGateVolumeImage = ConvertGateImageToITKImage(gate_image_volume);

  // Create projection images
  mPrimaryImage = CreateVoidProjectionImage();
  for(unsigned int i=0; i<PRIMARY; i++) {
    mProcessImage[ProcessType(i)] = CreateVoidProjectionImage();
    mSquaredImage[ProcessType(i)] = CreateVoidProjectionImage();
  }
  mSecondarySquaredImage = CreateVoidProjectionImage();

  mComptonPerOrderImages.clear();
  mRayleighPerOrderImages.clear();
  mFluorescencePerOrderImages.clear();

  // Set geometry from RTK geometry file
  if(mInputRTKGeometryFilename != "")
    SetGeometryFromInputRTKGeometryFile(mSource, mDetector, gate_image_volume, r);

  // Create geometry and param of output image
  ComputeGeometryInfoInImageCoordinateSystem(gate_image_volume,
                                             mDetector,
                                             mSource,
                                             mPrimarySourcePosition,
                                             mDetectorPosition,
                                             mDetectorRowVector,
                                             mDetectorColVector);

  // There are two geometry objects. One stores all projection images
  // (one per run) and the other contains the geometry of one projection
  // image.
  mGeometry->AddReg23Projection(mPrimarySourcePosition,
                                mDetectorPosition,
                                mDetectorRowVector,
                                mDetectorColVector);
  GeometryType::Pointer oneProjGeometry = GeometryType::New();
  oneProjGeometry->AddReg23Projection(mPrimarySourcePosition,
                                      mDetectorPosition,
                                      mDetectorRowVector,
                                      mDetectorColVector);

  // Create primary projector and compute primary
  mPrimaryProbe.Start();
  PrimaryProjectionType::Pointer primaryProjector = PrimaryProjectionType::New();
  primaryProjector->InPlaceOn();
  primaryProjector->SetInput(mPrimaryImage);
  primaryProjector->SetInput(1, mGateVolumeImage );
  primaryProjector->SetGeometry( oneProjGeometry.GetPointer() );
  primaryProjector->GetProjectedValueAccumulation().SetSolidAngleParameters(mPrimaryImage,
                                                                            mDetectorRowVector,
                                                                            mDetectorColVector);
  primaryProjector->GetProjectedValueAccumulation().SetVolumeSpacing( mGateVolumeImage->GetSpacing() );
  primaryProjector->GetProjectedValueAccumulation().SetInterpolationWeights( primaryProjector->GetInterpolationWeightMultiplication().GetInterpolationWeights() );
  primaryProjector->GetProjectedValueAccumulation().SetEnergyWeightList( &energyWeightList );
  primaryProjector->GetProjectedValueAccumulation().CreateMaterialMuMap(mEMCalculator,
                                                                        energyList,
                                                                        gate_image_volume);
  primaryProjector->GetProjectedValueAccumulation().Init( primaryProjector->GetNumberOfThreads() );
  primaryProjector->GetProjectedValueAccumulation().SetNumberOfPrimaries(mNoisePrimary);
  primaryProjector->GetProjectedValueAccumulation().SetResponseDetector( &mEnergyResponseDetector );
  TRY_AND_EXIT_ON_ITK_EXCEPTION(primaryProjector->Update());
  mPrimaryImage = primaryProjector->GetOutput();
  mPrimaryImage->DisconnectPipeline();

  // Compute flat field if required
  if(mAttenuationFilename != "" || mFlatFieldFilename != "") {
    // Constant image source of 1x1x1 voxel of world material
    typedef rtk::ConstantImageSource< InputImageType > ConstantImageSourceType;
    ConstantImageSourceType::PointType origin;
    ConstantImageSourceType::SizeType dim;
    ConstantImageSourceType::Pointer flatFieldSource  = ConstantImageSourceType::New();
    origin[0] = 0.;
    origin[1] = 0.;
    origin[2] = 0.;
    dim[0] = 1;
    dim[1] = 1;
    dim[2] = 1;
    flatFieldSource->SetOrigin( origin );
    flatFieldSource->SetSpacing( mGateVolumeImage->GetSpacing() );
    flatFieldSource->SetSize( dim );
    flatFieldSource->SetConstant( primaryProjector->GetProjectedValueAccumulation().GetMaterialMuMap()->GetLargestPossibleRegion().GetSize()[0]-1 );

    mFlatFieldImage = CreateVoidProjectionImage();
    primaryProjector->SetInput(mFlatFieldImage);
    primaryProjector->SetInput(1, flatFieldSource->GetOutput() );
    TRY_AND_EXIT_ON_ITK_EXCEPTION(primaryProjector->Update());
    mFlatFieldImage = primaryProjector->GetOutput();
  }
  mPrimaryProbe.Stop();

  // Prepare Compton
  mComptonProjector = ComptonProjectionType::New();
  mComptonProjector->InPlaceOn();
  mComptonProjector->SetInput(mProcessImage[COMPTON]);
  mComptonProjector->SetInput(1, mGateVolumeImage );
  mComptonProjector->SetGeometry( oneProjGeometry.GetPointer() );
  mComptonProjector->GetProjectedValueAccumulation().SetSolidAngleParameters(mProcessImage[COMPTON],
                                                                             mDetectorRowVector,
                                                                             mDetectorColVector);
  mComptonProjector->GetProjectedValueAccumulation().SetVolumeSpacing( mGateVolumeImage->GetSpacing() );
  mComptonProjector->GetProjectedValueAccumulation().SetInterpolationWeights( mComptonProjector->GetInterpolationWeightMultiplication().GetInterpolationWeights() );
  mComptonProjector->GetProjectedValueAccumulation().SetResponseDetector( &mEnergyResponseDetector );
  mComptonProjector->GetProjectedValueAccumulation().CreateMaterialMuMap(mEMCalculator,
                                                                         1.*keV,
                                                                         energyMax,
                                                                         gate_image_volume);
  mComptonProjector->GetProjectedValueAccumulation().Init( mComptonProjector->GetNumberOfThreads() );

  // Prepare Rayleigh
  mRayleighProjector = RayleighProjectionType::New();
  mRayleighProjector->InPlaceOn();
  mRayleighProjector->SetInput(mProcessImage[RAYLEIGH]);
  mRayleighProjector->SetInput(1, mGateVolumeImage );
  mRayleighProjector->SetGeometry( oneProjGeometry.GetPointer() );
  mRayleighProjector->GetProjectedValueAccumulation().SetSolidAngleParameters(mProcessImage[RAYLEIGH],
                                                                              mDetectorRowVector,
                                                                              mDetectorColVector);
  mRayleighProjector->GetProjectedValueAccumulation().SetVolumeSpacing( mGateVolumeImage->GetSpacing() );
  mRayleighProjector->GetProjectedValueAccumulation().SetInterpolationWeights( mRayleighProjector->GetInterpolationWeightMultiplication().GetInterpolationWeights() );
  mRayleighProjector->GetProjectedValueAccumulation().CreateMaterialMuMap(mEMCalculator,
                                                                         1.*keV,
                                                                         energyMax,
                                                                         gate_image_volume);
  mRayleighProjector->GetProjectedValueAccumulation().Init( mRayleighProjector->GetNumberOfThreads() );

  // Prepare Fluorescence
  mFluorescenceProjector = FluorescenceProjectionType::New();
  mFluorescenceProjector->InPlaceOn();
  mFluorescenceProjector->SetInput(mProcessImage[PHOTOELECTRIC]);
  mFluorescenceProjector->SetInput(1, mGateVolumeImage );
  mFluorescenceProjector->SetGeometry( oneProjGeometry.GetPointer() );
  mFluorescenceProjector->GetProjectedValueAccumulation().SetSolidAngleParameters(mProcessImage[PHOTOELECTRIC],
                                                                                  mDetectorRowVector,
                                                                                  mDetectorColVector);
  mFluorescenceProjector->GetProjectedValueAccumulation().SetVolumeSpacing( mGateVolumeImage->GetSpacing() );
  mFluorescenceProjector->GetProjectedValueAccumulation().SetInterpolationWeights( mFluorescenceProjector->GetInterpolationWeightMultiplication().GetInterpolationWeights() );
  mFluorescenceProjector->GetProjectedValueAccumulation().CreateMaterialMuMap(mEMCalculator,
                                                                         1.*keV,
                                                                         energyMax,
                                                                         gate_image_volume);
  mFluorescenceProjector->GetProjectedValueAccumulation().Init( mFluorescenceProjector->GetNumberOfThreads() );

  // Create a single event if asked for it
  if(mSingleInteractionFilename!="") {
      mInteractionPosition = mSingleInteractionPosition;
      mInteractionDirection = mSingleInteractionDirection;
      mInteractionEnergy = mSingleInteractionEnergy;
      mInteractionWeight = 1.;
      mInteractionZ = mSingleInteractionZ;
      mSingleInteractionImage = CreateVoidProjectionImage();
      if(mMapProcessNameWithType.find(mSingleInteractionType) == mMapProcessNameWithType.end())  {
        GateWarning("Unhandled gamma interaction in GateHybridForcedDetectionActor / single interaction. Process name is "
                    << mSingleInteractionType << ".\n");
      }
      else {
        switch(mMapProcessNameWithType[mSingleInteractionType]) {
        case COMPTON:
          this->ForceDetectionOfInteraction(mComptonProjector.GetPointer(),
                                            mSingleInteractionImage,
                                            mComptonPerOrderImages,
                                            mComptonProbe);
          break;
        case RAYLEIGH:
          mInteractionWeight = mEnergyResponseDetector(mInteractionEnergy)*mInteractionWeight;
          this->ForceDetectionOfInteraction(mRayleighProjector.GetPointer(),
                                            mSingleInteractionImage,
                                            mRayleighPerOrderImages,
                                            mRayleighProbe);
          break;
        case PHOTOELECTRIC:
          mInteractionWeight = mEnergyResponseDetector(mInteractionEnergy)*mInteractionWeight;
          this->ForceDetectionOfInteraction(mFluorescenceProjector.GetPointer(),
                                            mSingleInteractionImage,
                                            mFluorescencePerOrderImages,
                                            mFluorescenceProbe);
          break;
        default:
          GateError("Implementation problem, unexpected process type reached.");
        }
      }
  }

  if(mWaterLUTFilename != "")
    CreateWaterLUT(energyList, energyWeightList);

  if(mIsSecondarySquaredImageEnabled || mIsSecondaryUncertaintyImageEnabled) {
    for(unsigned int i=0; i<PRIMARY; i++)
      mEventImage[ProcessType(i)] = CreateVoidProjectionImage();
  }

  if(mSecondPassPrefix != "") {
    for(unsigned int i=0; i<PRIMARY; i++)
      {
      mRussianRouletteImages[ProcessType(i)]      = CreateRussianRouletteVoidImage();
      mRussianRouletteCountImages[ProcessType(i)] = CreateRussianRouletteVoidImage();
      }
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Callback Begin of Run
void GateHybridForcedDetectionActor::EndOfRunAction(const G4Run*r)
{
  // Compute sum of variance per process
  std::map<ProcessType, G4double> varPerProcess;
  if(mSecondPassPrefix != "") {
    G4double maxVarPerProcess = 0.;
    double invN = 1./mNumberOfEventsInRun;
    for(unsigned int i=0; i<PRIMARY; i++) {
      ProcessType p = ProcessType(i);
      varPerProcess[p] = 0.;
      itk::ImageRegionIterator<OutputImageType> itp(mProcessImage[p],
                                                    mProcessImage[p]->GetBufferedRegion());
      itk::ImageRegionIterator<OutputImageType> its(mSquaredImage[p],
                                                    mSquaredImage[p]->GetBufferedRegion());
      for(; !itp.IsAtEnd(); ++itp, ++its) {
        varPerProcess[p] += its.Get()*invN - pow(itp.Get()*invN, 2.);
      }
      maxVarPerProcess = std::max(maxVarPerProcess, varPerProcess[p]);

      // Compute survival probability image for Russian Roulette
      // Max of russian roulette image
      itk::ImageRegionIterator<OutputImageType> iti(mRussianRouletteImages[p],
                                                    mRussianRouletteImages[p]->GetBufferedRegion());
      float maxRussian = 0.;
      for(iti.GoToBegin(); !iti.IsAtEnd(); ++iti) {
        maxRussian = std::max(maxRussian, iti.Get());
      }

      itk::ImageRegionIterator<OutputImageType> itc(mRussianRouletteCountImages[p],
                                                    mRussianRouletteCountImages[p]->GetBufferedRegion());
      mRussianRouletteImagesProbability[p] = CreateRussianRouletteVoidImage();
      itk::ImageRegionIterator<OutputImageType> itr(mRussianRouletteImagesProbability[p],
                                                    mRussianRouletteImagesProbability[p]->GetBufferedRegion());
      for(iti.GoToBegin(); !iti.IsAtEnd(); ++iti, ++itc, ++itr) {
        double survivalProba = 1.;
        if(itc.Get()>mRussianRouletteMinimumCountInRegion) {
          survivalProba = iti.Get() / maxRussian;
          survivalProba = std::min(survivalProba, 1.);
          survivalProba = std::max(survivalProba, mRussianRouletteMinimumProbability);
        }
        itr.Set(survivalProba);
      }
    }
    // Normalize by max
    for(unsigned int i=0; i<PRIMARY; i++) {
      ProcessType p = ProcessType(i);
      varPerProcess[p] /= maxVarPerProcess;
      std::cout << "Process " << mMapTypeWithProcessName[p]
                << " probability " << varPerProcess[p]
                << std::endl;
    }
  }

  // Save data
  GateVActor::EndOfRunAction(r);

  if(mSecondPassPrefix != "") {
    // Init
    GeometryType::Pointer geoBackup = GeometryType::New();
    std::swap(mGeometry, geoBackup);
    std::swap(mSecondPassDetectorResolution, mDetectorResolution);
    unsigned int backupNumberOfEventsInRun = mNumberOfEventsInRun;
    int currentEvent=-1;
    BeginOfRunAction(r);

    // Let's kill
    for(int i=0; i<mPhaseSpace->GetEntries(); i++, mPhaseSpace->GetEntry(i)) {
      // Init
      if(mInteractionEventId!=currentEvent) {
        if(currentEvent>=0) EndOfEventAction();
        BeginOfEventAction();
        currentEvent = mInteractionEventId;
      }

      // Convert to ITK
      G4ThreeVector p = m_WorldToCT.TransformPoint(mInteractionPosition);
      PointType point;
      for(unsigned int i=0; i<3; i++)
        point[i] = p[i];
      OutputImageType::IndexType idx;

      // Russian roulette
      ProcessType pt = mMapProcessNameWithType[mInteractionProductionProcessStep];
      mRussianRouletteImagesProbability[pt]->TransformPhysicalPointToIndex(point, idx);
      double survivalProba = 1.;
      if(mRussianRouletteImagesProbability[pt]->GetBufferedRegion().IsInside(idx))
        survivalProba = mRussianRouletteImagesProbability[pt]->GetPixel(idx) * varPerProcess[pt];
      if(G4UniformRand()>survivalProba) {
        mInteractionWeight = 0.;
      }
      else {
        mInteractionWeight /= survivalProba;

        // Interaction survived, let's do the job
        switch(pt) {
        case COMPTON:
          this->ForceDetectionOfInteraction(mComptonProjector.GetPointer(),
                                            mProcessImage[COMPTON],
                                            mComptonPerOrderImages,
                                            mComptonProbe);
          break;
        case RAYLEIGH:
          this->ForceDetectionOfInteraction(mRayleighProjector.GetPointer(),
                                            mProcessImage[RAYLEIGH],
                                            mRayleighPerOrderImages,
                                            mRayleighProbe);
          break;
        case PHOTOELECTRIC:
          this->ForceDetectionOfInteraction(mFluorescenceProjector.GetPointer(),
                                            mProcessImage[PHOTOELECTRIC],
                                            mFluorescencePerOrderImages,
                                            mFluorescenceProbe);
          break;
        default:
          GateError("Implementation problem, unexpected process type reached.");
        }
      }
      if(mSecondPassPhaseSpaceFile) mSecondPassPhaseSpace->Fill();
    }
    EndOfEventAction();
    std::swap(mNumberOfEventsInRun, backupNumberOfEventsInRun);
    SaveData(mSecondPassPrefix);
    std::swap(mGeometry, geoBackup);
    std::swap(mSecondPassDetectorResolution, mDetectorResolution);
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void GateHybridForcedDetectionActor::BeginOfEventAction(const G4Event *itkNotUsed(e))
{
  mNumberOfEventsInRun++;

  if(mIsSecondarySquaredImageEnabled || mIsSecondaryUncertaintyImageEnabled) {
    // The event contribution are put in new images which at this point are in the
    // mEventComptonImage / mEventRayleighImage / mEventFluorescenceImage. We therefore
    // swap the two and they will be swapped back in EndOfEventAction.
    for(unsigned int i=0; i<PRIMARY; i++) {
      std::swap(mEventImage[ProcessType(i)], mProcessImage[ProcessType(i)]);
      // Make sure the time stamps of the mEvent images are more recent to detect if one
      // image has been modified during the event.
      mEventImage[ProcessType(i)]->Modified();
    }
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void GateHybridForcedDetectionActor::EndOfEventAction(const G4Event *e)
{
  if(mIsSecondarySquaredImageEnabled || mIsSecondaryUncertaintyImageEnabled) {
    typedef itk::AddImageFilter <OutputImageType, OutputImageType, OutputImageType> AddImageFilterType;
    AddImageFilterType::Pointer addFilter = AddImageFilterType::New();
    typedef itk::MultiplyImageFilter<OutputImageType, OutputImageType, OutputImageType> MultiplyImageFilterType;
    MultiplyImageFilterType::Pointer multFilter = MultiplyImageFilterType::New();

    // First: accumulate contribution to event, square and add to total squared
    InputImageType::Pointer totalContribEvent(NULL);
    for(unsigned int i=0; i<PRIMARY; i++) {
      if( mEventImage[ProcessType(i)]->GetTimeStamp() < mProcessImage[ProcessType(i)]->GetTimeStamp() ) {
        // First: accumulate contribution to event, square and add to total squared
        multFilter->SetInput1(mProcessImage[ProcessType(i)]);
        multFilter->SetInput2(mProcessImage[ProcessType(i)]);
        multFilter->InPlaceOff();
        addFilter->SetInput1(mSquaredImage[ProcessType(i)]);
        addFilter->SetInput2(multFilter->GetOutput());
        addFilter->InPlaceOff();
        TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
        mSquaredImage[ProcessType(i)] = addFilter->GetOutput();
        mSquaredImage[ProcessType(i)]->DisconnectPipeline();

        // Second: accumulate in total event for the global secondary image
        if(totalContribEvent.GetPointer()) {
          addFilter->SetInput1(totalContribEvent);
          addFilter->SetInput2(mProcessImage[ProcessType(i)]);
          TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
          totalContribEvent = addFilter->GetOutput();
          totalContribEvent->DisconnectPipeline();
        }
        else
          totalContribEvent = mProcessImage[ProcessType(i)];
      }
    }

    if(totalContribEvent.GetPointer()) {
      multFilter->SetInput1(totalContribEvent);
      multFilter->SetInput2(totalContribEvent);
      multFilter->InPlaceOff();
      addFilter->SetInput1(mSecondarySquaredImage);
      addFilter->SetInput2(multFilter->GetOutput());
      TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
      mSecondarySquaredImage = addFilter->GetOutput();
      mSecondarySquaredImage->DisconnectPipeline();
    }

    for(unsigned int i=0; i<PRIMARY; i++) {
      if( mEventImage[ProcessType(i)]->GetTimeStamp() < mProcessImage[ProcessType(i)]->GetTimeStamp() ) {
        // Accumulate non squared images and reset mEvent images
        addFilter->SetInput1(mEventImage[ProcessType(i)]);
        addFilter->SetInput2(mProcessImage[ProcessType(i)]);
        TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
        mProcessImage[ProcessType(i)] = addFilter->GetOutput();
        mProcessImage[ProcessType(i)]->DisconnectPipeline();
        mEventImage[ProcessType(i)] = CreateVoidProjectionImage();
      }
      else
        std::swap(mEventImage[ProcessType(i)], mProcessImage[ProcessType(i)]);
    }
  }
  if(e)
    GateVActor::EndOfEventAction(e);
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Callbacks
void GateHybridForcedDetectionActor::UserSteppingAction(const GateVVolume * v,
                                                        const G4Step * step)
{
  GateVActor::UserSteppingAction(v, step);
  /* Get interaction point from step
     Retrieve :
     - type of limiting process (Compton Rayleigh Fluorescence)
     - coordinate of interaction, convert if needed into world coordinate system
     - Get Energy
     - -> generate adequate forward projections towards detector
  */

  // We are only interested in EM processes. One should check post-step to know
  // what is going to happen, but pre-step is used afterward to get direction
  // and position.
  const G4VProcess *pr = step->GetPostStepPoint()->GetProcessDefinedStep();
  const G4VEmProcess *process = dynamic_cast<const G4VEmProcess*>(pr);
  if(!process) return;

  GateScatterOrderTrackInformation * info = dynamic_cast<GateScatterOrderTrackInformation *>(step->GetTrack()->GetUserInformation());
  int order = (info)?info->GetScatterOrder():-1;

  //FIXME: do we prefer this solution or computing the scattering function for the material?
  const G4MaterialCutsCouple *couple = step->GetPreStepPoint()->GetMaterialCutsCouple();
  const G4ParticleDefinition *particle = step->GetTrack()->GetParticleDefinition();
#if G4VERSION_MAJOR<10 && G4VERSION_MINOR==5
  G4VEmModel* model = const_cast<G4VEmProcess*>(process)->Model();
#else
  G4VEmModel* model = const_cast<G4VEmProcess*>(process)->EmModel();
#endif
  const G4Element* elm = model->SelectRandomAtom(couple,
                                                 particle,
                                                 step->GetPreStepPoint()->GetKineticEnergy());

  if(process->GetProcessName() == G4String("PhotoElectric") ||
     process->GetProcessName() == G4String("phot")) {

    // List of secondary particles
    const G4TrackVector * list = step->GetSecondary();

    for(unsigned int i = 0; i<(*list).size(); i++) {
      G4String nameSecondary = (*list)[i]->GetDefinition()->GetParticleName();

      // Check if photon has been emitted
      if(nameSecondary==G4String("gamma")) {
        GateScatterOrderTrackInformation * infoSecondary = dynamic_cast<GateScatterOrderTrackInformation *>((*list)[i]->GetUserInformation());
        order = (info)?infoSecondary->GetScatterOrder():-1;

        ForceDetectionOfInteraction(GateRunManager::GetRunManager()->GetCurrentRun()->GetRunID(),
                                    GateRunManager::GetRunManager()->GetCurrentEvent()->GetEventID(),
                                    step->GetTrack()->GetTrackID(),
                                    (step->GetTrack()->GetLogicalVolumeAtVertex())?step->GetTrack()->GetLogicalVolumeAtVertex()->GetName():"",
                                    (step->GetTrack()->GetCreatorProcess())?step->GetTrack()->GetCreatorProcess()->GetProcessName():"",
                                    process->GetProcessName(),
                                    step->GetPreStepPoint()->GetPhysicalVolume()->GetLogicalVolume()->GetName(),
                                    step->GetPostStepPoint()->GetPosition(),
                                    (*list)[i]->GetMomentumDirection(),
                                    (*list)[i]->GetKineticEnergy(),
                                    (*list)[i]->GetWeight(),
                                    step->GetPreStepPoint()->GetMaterial()->GetName(),
                                    elm->GetZ(),
                                    order);
      }
    }
  }
  else
    ForceDetectionOfInteraction(GateRunManager::GetRunManager()->GetCurrentRun()->GetRunID(),
                                GateRunManager::GetRunManager()->GetCurrentEvent()->GetEventID(),
                                step->GetTrack()->GetTrackID(),
                                (step->GetTrack()->GetLogicalVolumeAtVertex())?step->GetTrack()->GetLogicalVolumeAtVertex()->GetName():"",
                                (step->GetTrack()->GetCreatorProcess())?step->GetTrack()->GetCreatorProcess()->GetProcessName():"",
                                process->GetProcessName(),
                                step->GetPreStepPoint()->GetPhysicalVolume()->GetLogicalVolume()->GetName(),
                                step->GetPostStepPoint()->GetPosition(),
                                step->GetPreStepPoint()->GetMomentumDirection(),
                                step->GetPreStepPoint()->GetKineticEnergy(),
                                step->GetPostStepPoint()->GetWeight(),
                                step->GetPreStepPoint()->GetMaterial()->GetName(),
                                elm->GetZ(),
                                order);
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
/// Save data
void GateHybridForcedDetectionActor::ForceDetectionOfInteraction(G4int runID,
                                                                 G4int eventID,
                                                                 G4int trackID,
                                                                 G4String prodVol,
                                                                 G4String creatorProc,
                                                                 G4String processName,
                                                                 G4String interVol,
                                                                 G4ThreeVector pt,
                                                                 G4ThreeVector dir,
                                                                 double energy,
                                                                 double weight,
                                                                 G4String material,
                                                                 int Z,
                                                                 int order)
{
  // In case a root file is created, copy values to branched variables
  mInteractionPosition = pt;
  mInteractionDirection = dir;
  mInteractionEnergy = energy;
  mInteractionWeight = weight;
  mInteractionZ = Z;
  mInteractionRunId   = runID;
  mInteractionEventId = eventID;
  mInteractionTrackId = trackID;
  strcpy(mInteractionProductionVolume, prodVol.c_str());
  strcpy(mInteractionProductionProcessTrack, creatorProc.c_str());
  strcpy(mInteractionProductionProcessStep, processName.c_str());
  strcpy(mInteractionVolume, interVol.c_str());
  strcpy(mInteractionMaterial, material.c_str());
  mInteractionOrder = order;

  if(mMapProcessNameWithType.find(processName) == mMapProcessNameWithType.end())  {
    GateWarning("Unhandled gamma interaction in GateHybridForcedDetectionActor. Process name is "
                << processName << ".\n");
    return;
  }
  else {
    switch(mMapProcessNameWithType[processName]) {
    case COMPTON:
      this->ForceDetectionOfInteraction(mComptonProjector.GetPointer(),
                                        mProcessImage[COMPTON],
                                        mComptonPerOrderImages,
                                        mComptonProbe);
      break;
    case RAYLEIGH:
      mInteractionWeight = mEnergyResponseDetector(mInteractionEnergy)*mInteractionWeight;
      this->ForceDetectionOfInteraction(mRayleighProjector.GetPointer(),
                                        mProcessImage[RAYLEIGH],
                                        mRayleighPerOrderImages,
                                        mRayleighProbe);
      break;
    case PHOTOELECTRIC:
      mInteractionWeight = mEnergyResponseDetector(mInteractionEnergy)*mInteractionWeight;
      this->ForceDetectionOfInteraction(mFluorescenceProjector.GetPointer(),
                                        mProcessImage[PHOTOELECTRIC],
                                        mFluorescencePerOrderImages,
                                        mFluorescenceProbe);
      break;
    default:
      GateError("Implementation problem, unexpected process type reached.");
    }
  }
  if(mPhaseSpaceFile) mPhaseSpace->Fill();
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template <class TProjectorType>
void GateHybridForcedDetectionActor::ForceDetectionOfInteraction(TProjectorType *projector,
                                                                 InputImageType::Pointer &input,
                                                                 std::vector<InputImageType::Pointer> &inputPerOrder,
                                                                 itk::TimeProbe &probe)
{
  // d and p are in World coordinates and they must be in CT coordinates
  G4ThreeVector p = m_WorldToCT.TransformPoint(mInteractionPosition);
  G4ThreeVector d = m_WorldToCT.TransformAxis(mInteractionDirection);

  // Convert to ITK
  PointType point;
  VectorType direction;
  for(unsigned int i=0; i<3; i++) {
    point[i] = p[i];
    direction[i] = d[i];
  }

  // Create interaction geometry
  GeometryType::Pointer oneProjGeometry = GeometryType::New();
  oneProjGeometry->AddReg23Projection(point,
                                      mDetectorPosition,
                                      mDetectorRowVector,
                                      mDetectorColVector);

  probe.Start();
  projector->SetInput(input);
  projector->SetGeometry( oneProjGeometry.GetPointer() );
  projector->GetProjectedValueAccumulation().SetEnergyZAndWeight( mInteractionEnergy, mInteractionZ, mInteractionWeight );
  projector->GetProjectedValueAccumulation().SetDirection( direction );
  TRY_AND_EXIT_ON_ITK_EXCEPTION(projector->Update());
  input = projector->GetOutput();
  input->DisconnectPipeline();
  probe.Stop();
  mInteractionTotalContribution = projector->GetProjectedValueAccumulation().GetIntegralOverDetectorAndReset();
  if(mSecondPassPrefix != "") {
    OutputImageType::IndexType idx;
    ProcessType pt = mMapProcessNameWithType[mInteractionProductionProcessStep];
    mRussianRouletteImages[pt]->TransformPhysicalPointToIndex(point, idx);
    if(mRussianRouletteImages[pt]->GetBufferedRegion().IsInside(idx)) {
      mRussianRouletteImages[pt]->SetPixel(idx,
                                           mRussianRouletteImages[pt]->GetPixel(idx) + mInteractionTotalContribution);
      mRussianRouletteCountImages[pt]->SetPixel(idx,
                                                mRussianRouletteCountImages[pt]->GetPixel(idx) + 1.);
    }
  }

  // Scatter order
  if(mInteractionOrder>=0)
  {
    while(mInteractionOrder>=(int)inputPerOrder.size())
      inputPerOrder.push_back( CreateVoidProjectionImage() );
    projector->SetInput(inputPerOrder[mInteractionOrder]);
    TRY_AND_EXIT_ON_ITK_EXCEPTION(projector->Update());
    inputPerOrder[mInteractionOrder] = projector->GetOutput();
    inputPerOrder[mInteractionOrder]->DisconnectPipeline();
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
/// Save data
void GateHybridForcedDetectionActor::SaveData(const G4String prefix)
{
  typedef itk::BinaryFunctorImageFilter< InputImageType, InputImageType, InputImageType,
                                         GateHybridForcedDetectionFunctor::Chetty<InputImageType::PixelType> > ChettyType;

  GateVActor::SaveData();

  // Geometry
  if(mGeometryFilename != "") {
    rtk::ThreeDCircularProjectionGeometryXMLFileWriter::Pointer geoWriter =
        rtk::ThreeDCircularProjectionGeometryXMLFileWriter::New();
    geoWriter->SetObject(mGeometry);
    geoWriter->SetFilename(AddPrefix(prefix,mGeometryFilename));
    geoWriter->WriteFile();
  }

  itk::ImageFileWriter<InputImageType>::Pointer imgWriter;
  imgWriter = itk::ImageFileWriter<InputImageType>::New();
  char filename[1024];
  G4int rID = G4RunManager::GetRunManager()->GetCurrentRun()->GetRunID();

  if(mPrimaryFilename != "") {
    // Write the image of primary radiation accounting for the fluence of the
    // primary source.
    sprintf(filename, AddPrefix(prefix,mPrimaryFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput( PrimaryFluenceWeighting(mPrimaryImage) );
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
  }

  if(mMaterialMuFilename != "") {
    AccumulationType::MaterialMuImageType *map;
    map = mComptonProjector->GetProjectedValueAccumulation().GetMaterialMu();

    // Change spacing to keV
    AccumulationType::MaterialMuImageType::SpacingType spacing = map->GetSpacing();
    spacing[1] /= keV;

    typedef itk::ChangeInformationImageFilter<AccumulationType::MaterialMuImageType> CIType;
    CIType::Pointer ci = CIType::New();
    ci->SetInput(map);
    ci->SetOutputSpacing(spacing);
    ci->ChangeSpacingOn();
    ci->Update();

    typedef itk::ImageFileWriter<AccumulationType::MaterialMuImageType> TwoDWriter;
    TwoDWriter::Pointer w = TwoDWriter::New();
    w->SetInput( ci->GetOutput() );
    w->SetFileName(AddPrefix(prefix,mMaterialMuFilename));
    TRY_AND_EXIT_ON_ITK_EXCEPTION(w->Update());
  }

  if(mAttenuationFilename != "") {
    //Attenuation Functor -> atten
    typedef itk::BinaryFunctorImageFilter< InputImageType, InputImageType, InputImageType,
                                           GateHybridForcedDetectionFunctor::Attenuation<InputImageType::PixelType> > attenFunctor;
    attenFunctor::Pointer atten = attenFunctor::New();

    // In the attenuation, we assume that the whole detector is irradiated.
    // Otherwise we would have a division by 0.
    atten->SetInput1(mPrimaryImage);
    atten->SetInput2(mFlatFieldImage);
    atten->InPlaceOff();

    sprintf(filename, AddPrefix(prefix,mAttenuationFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput(atten->GetOutput());
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
  }

  if(mFlatFieldFilename != "") {
    // Write the image of the flat field accounting for the fluence of the
    // primary source.
    sprintf(filename, AddPrefix(prefix,mFlatFieldFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput( PrimaryFluenceWeighting(mFlatFieldImage) );
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
  }

  if(mComptonFilename != "") {
    sprintf(filename, AddPrefix(prefix,mComptonFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput(mProcessImage[COMPTON]);
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    if(mIsSecondarySquaredImageEnabled) {
      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Squared." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(mSquaredImage[COMPTON]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
    if(mIsSecondaryUncertaintyImageEnabled) {
      ChettyType::Pointer chetty = ChettyType::New();
      chetty->GetFunctor().SetN(mNumberOfEventsInRun);
      chetty->SetInput1(mProcessImage[COMPTON]);
      chetty->SetInput2(mSquaredImage[COMPTON]);
      chetty->InPlaceOff();

      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Uncertainty." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(chetty->GetOutput());

      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    for(unsigned int k = 1; k<mComptonPerOrderImages.size(); k++)
    {
      sprintf(filename, AddPrefix(prefix,"output/compton%04d_order%02d.mha").c_str(), rID, k);
      imgWriter->SetFileName(filename);
      imgWriter->SetInput(mComptonPerOrderImages[k]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
  }

  if(mRayleighFilename != "") {
    sprintf(filename, AddPrefix(prefix,mRayleighFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput(mProcessImage[RAYLEIGH]);
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    if(mIsSecondarySquaredImageEnabled) {
      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Squared." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(mSquaredImage[RAYLEIGH]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
    if(mIsSecondaryUncertaintyImageEnabled) {
      ChettyType::Pointer chetty = ChettyType::New();
      chetty->GetFunctor().SetN(mNumberOfEventsInRun);
      chetty->SetInput1(mProcessImage[RAYLEIGH]);
      chetty->SetInput2(mSquaredImage[RAYLEIGH]);
      chetty->InPlaceOff();

      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Uncertainty." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(chetty->GetOutput());

      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    for(unsigned int k = 1; k<mRayleighPerOrderImages.size(); k++)
    {
      sprintf(filename, AddPrefix(prefix,"output/rayleigh%04d_order%02d.mha").c_str(), rID, k);
      imgWriter->SetFileName(filename);
      imgWriter->SetInput(mRayleighPerOrderImages[k]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
  }

  if(mFluorescenceFilename != "") {
    sprintf(filename, AddPrefix(prefix,mFluorescenceFilename).c_str(), rID);
    imgWriter->SetFileName(filename);
    imgWriter->SetInput(mProcessImage[PHOTOELECTRIC]);
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    if(mIsSecondarySquaredImageEnabled) {
      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Squared." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(mSquaredImage[PHOTOELECTRIC]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
    if(mIsSecondaryUncertaintyImageEnabled) {
      ChettyType::Pointer chetty = ChettyType::New();
      chetty->GetFunctor().SetN(mNumberOfEventsInRun);
      chetty->SetInput1(mProcessImage[PHOTOELECTRIC]);
      chetty->SetInput2(mSquaredImage[PHOTOELECTRIC]);
      chetty->InPlaceOff();

      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Uncertainty." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(chetty->GetOutput());

      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    for(unsigned int k = 1; k<mFluorescencePerOrderImages.size(); k++)
    {
      sprintf(filename, AddPrefix(prefix,"output/fluorescence%04d_order%02d.mha").c_str(), rID, k);
      imgWriter->SetFileName(filename);
      imgWriter->SetInput(mFluorescencePerOrderImages[k]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
  }

  if(mSecondaryFilename != "" ||
     mIsSecondarySquaredImageEnabled ||
     mIsSecondaryUncertaintyImageEnabled ||
     mTotalFilename != "") {
    typedef itk::AddImageFilter <OutputImageType, OutputImageType, OutputImageType> AddImageFilterType;
    AddImageFilterType::Pointer addFilter = AddImageFilterType::New();

    // The secondary image contains all calculated scatterings
    // (Compton, Rayleigh and/or Fluorescence)
    // Create projections image
    InputImageType::Pointer mSecondaryImage = CreateVoidProjectionImage();
    for(unsigned int i=0; i<PRIMARY; i++) {
      // Add Image Filter used to sum the different figures obtained on each process
      addFilter->InPlaceOn();
      addFilter->SetInput1(mSecondaryImage);
      addFilter->SetInput2(mProcessImage[ProcessType(i)]);
      TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
      mSecondaryImage = addFilter->GetOutput();
      mSecondaryImage->DisconnectPipeline();
    }

    // Write scatter image
    if(mSecondaryFilename != "") {
      sprintf(filename, AddPrefix(prefix,mSecondaryFilename).c_str(), rID);
      imgWriter->SetFileName(filename);
      imgWriter->SetInput(mSecondaryImage);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    if(mIsSecondarySquaredImageEnabled) {
      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Squared." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(mSecondarySquaredImage);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    // Write scatter uncertainty image
    if(mIsSecondaryUncertaintyImageEnabled) {
      ChettyType::Pointer chetty = ChettyType::New();
      chetty->GetFunctor().SetN(mNumberOfEventsInRun);
      chetty->SetInput1(mSecondaryImage);
      chetty->SetInput2(mSecondarySquaredImage);
      chetty->InPlaceOff();

      imgWriter->SetFileName(G4String(removeExtension(filename)) +
                             "-Uncertainty." +
                             G4String(getExtension(filename)));
      imgWriter->SetInput(chetty->GetOutput());

      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }

    if(mTotalFilename != "") {
      // Primary
      mSecondaryImage->DisconnectPipeline();
      addFilter->SetInput1(mSecondaryImage);
      addFilter->SetInput2(mPrimaryImage);
      TRY_AND_EXIT_ON_ITK_EXCEPTION( addFilter->Update() );
      mSecondaryImage = addFilter->GetOutput();

      // Write Total Image
      sprintf(filename, AddPrefix(prefix,mTotalFilename).c_str(), rID);
      imgWriter->SetFileName(filename);
      imgWriter->SetInput(mSecondaryImage);
      TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
    }
  }

  if(mSingleInteractionFilename!="") {
    imgWriter->SetFileName(AddPrefix(prefix,mSingleInteractionFilename));
    imgWriter->SetInput(mSingleInteractionImage);
    TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
  }

  if(mPhaseSpaceFile)
    mPhaseSpace->GetCurrentFile()->Write();
  if(mSecondPassPhaseSpaceFile)
    mSecondPassPhaseSpace->GetCurrentFile()->Write();
  if(mRussianRouletteFilename) {
    G4String f = mRussianRouletteFilename;
    if(mSecondPassPrefix != "") {
      for(unsigned int i=0; i<PRIMARY; i++)
        {
        G4String base = G4String(removeExtension(f));
        G4String ext = G4String(getExtension(f));
        ProcessType pt = ProcessType(i);
        imgWriter->SetFileName(AddPrefix(prefix,
                               AddPrefix(mMapTypeWithProcessName[pt]+'-', f)));
        imgWriter->SetInput(mRussianRouletteImages[pt]);
        TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());

        imgWriter->SetFileName(AddPrefix(prefix,
                               AddPrefix(mMapTypeWithProcessName[pt]+'-', base + "-Count." + ext)));
        imgWriter->SetInput(mRussianRouletteCountImages[pt]);
        TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());

        imgWriter->SetFileName( AddPrefix(prefix,
                                AddPrefix(mMapTypeWithProcessName[pt]+'-', base + "-Probability." + ext)));
        imgWriter->SetInput(mRussianRouletteImagesProbability[pt]);
        TRY_AND_EXIT_ON_ITK_EXCEPTION(imgWriter->Update());
        }
    }
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void GateHybridForcedDetectionActor::ResetData()
{
  mGeometry = GeometryType::New();
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void
GateHybridForcedDetectionActor::
SetGeometryFromInputRTKGeometryFile(GateVSource *source,
                                    GateVVolume *detector,
                                    GateVImageVolume *ct,
                                    const G4Run *run)
{
  if( ! mInputGeometry.GetPointer() ) {
    rtk::ThreeDCircularProjectionGeometryXMLFileReader::Pointer geometryReader;
    geometryReader = rtk::ThreeDCircularProjectionGeometryXMLFileReader::New();
    geometryReader->SetFilename(mInputRTKGeometryFilename);
    TRY_AND_EXIT_ON_ITK_EXCEPTION( geometryReader->GenerateOutputInformation() );
    mInputGeometry = geometryReader->GetGeometry();
  }

  if( run->GetRunID() >= (int) mInputGeometry->GetGantryAngles().size() ) {
    GateError("SetGeometryFromInputRTKGeometryFile: you have more runs than what file "
              << mInputRTKGeometryFilename << " describes.");
  }

  // Source
  if( source->GetRelativePlacementVolume() != "world") {
    GateError("SetGeometryFromInputRTKGeometryFile"
              << "expects a source attached to the world.");
  }
  G4ThreeVector srcTrans;
  srcTrans[0] = mInputGeometry->GetSourceOffsetsX()[ run->GetRunID() ];
  srcTrans[1] = mInputGeometry->GetSourceOffsetsY()[ run->GetRunID() ];
  srcTrans[2] = mInputGeometry->GetSourceToIsocenterDistances()[ run->GetRunID() ];
  if(source->GetPosDist()->GetPosDisType() == "Point")
    source->GetPosDist()->SetCentreCoords(srcTrans); // point
  else {
    G4ThreeVector offset = source->GetPosDist()->GetCentreCoords() -
                           source->GetAngDist()->GetFocusPointCopy();
    source->GetAngDist()->SetFocusPoint(srcTrans);
    source->GetAngDist()->SetFocusPointCopy(srcTrans);
    source->GetPosDist()->SetCentreCoords(srcTrans+offset);
  }

  // Detector
  if( detector->GetParentVolume()->GetLogicalVolume()->GetName() != "world_log" ) {
    GateError("SetGeometryFromInputRTKGeometryFile"
              << " expects a detector attached to the world.");
  }
  G4ThreeVector detTrans;
  //FIXME: detector->GetOrigin()?
  detTrans[0] = mInputGeometry->GetProjectionOffsetsX()[ run->GetRunID() ];
  detTrans[1] = mInputGeometry->GetProjectionOffsetsY()[ run->GetRunID() ];
  detTrans[2] = srcTrans[2] - mInputGeometry->GetSourceToDetectorDistances()[ run->GetRunID() ];
  detector->GetPhysicalVolume()->SetTranslation(detTrans);

  // Create rotation matrix and rotate CT
  if( ct->GetParentVolume()->GetLogicalVolume()->GetName() != "world_log" ) {
    GateError("SetGeometryFromInputRTKGeometryFile"
              << " expects a voxelized volume attached to the world.");
  }
  CLHEP::Hep3Vector rows[3];
  for(unsigned int j=0; j<3; j++)
    for(unsigned int i=0; i<3; i++)
      rows[j][i] = mInputGeometry->GetRotationMatrices()[ run->GetRunID() ](i, j);
  if( ! ct->GetPhysicalVolume()->GetRotation() )
    ct->GetPhysicalVolume()->SetRotation(new G4RotationMatrix);
  ct->GetPhysicalVolume()->GetRotation()->setRows(rows[0], rows[1], rows[2]);

  // According to BookForAppliDev.pdf section 3.4.4.3, we are allowed to change
  // the geometry in BeginOfRunAction provided that we call this:
  GateRunManager::GetRunManager()->GeometryHasBeenModified();
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void GateHybridForcedDetectionActor::ComputeGeometryInfoInImageCoordinateSystem(
        GateVImageVolume *ct,
        GateVVolume *detector,
        GateVSource *src,
        PointType &primarySourcePosition,
        PointType &detectorPosition,
        VectorType &detectorRowVector,
        VectorType &detectorColVector)
{
  // The placement of a volume relative to its mother's coordinate system is not
  // very well explained in Geant4's doc but the code follows what's done in
  // source/geometry/volumes/src/G4PVPlacement.cc.
  //
  // One must be extremely careful with the multiplication order. It is not
  // intuitive in Geant4, i.e., G4AffineTransform.Product(A, B) means
  // B*A in matrix notations.

  // Detector to world
  GateVVolume * v = detector;
  G4VPhysicalVolume * phys = v->GetPhysicalVolume();
  G4AffineTransform detectorToWorld(phys->GetRotation(), phys->GetTranslation());

  while (v->GetLogicalVolumeName() != "world_log") {
    v = v->GetParentVolume();
    phys = v->GetPhysicalVolume();
    G4AffineTransform x(phys->GetRotation(), phys->GetTranslation());
    detectorToWorld = detectorToWorld * x;
  }

  // CT to world
  v = ct;
  phys = v->GetPhysicalVolume();
  G4AffineTransform ctToWorld(phys->GetRotation(), phys->GetTranslation());
  while (v->GetLogicalVolumeName() != "world_log") {
    v = v->GetParentVolume();
    phys = v->GetPhysicalVolume();
    G4AffineTransform x(phys->GetRotation(), phys->GetTranslation());
    ctToWorld = ctToWorld * x;
  }
  m_WorldToCT = ctToWorld.Inverse();

  // Source to world
  G4String volname = src->GetRelativePlacementVolume();
  v = GateObjectStore::GetInstance()->FindVolumeCreator(volname);
  phys = v->GetPhysicalVolume();
  G4AffineTransform sourceToWorld(phys->GetRotation(), phys->GetTranslation());
  while (v->GetLogicalVolumeName() != "world_log") {
    v = v->GetParentVolume();
    phys = v->GetPhysicalVolume();
    G4AffineTransform x(phys->GetRotation(), phys->GetTranslation());
    sourceToWorld = sourceToWorld * x;
  }

  // Detector parameters
  G4AffineTransform detectorToCT(detectorToWorld *  m_WorldToCT);

  // TODO: check where to get the two directions of the detector.
  // Probably the dimension that has lowest size in one of the three directions.
  G4ThreeVector du = detectorToCT.TransformAxis(G4ThreeVector(1,0,0));
  G4ThreeVector dv = detectorToCT.TransformAxis(G4ThreeVector(0,1,0));
  G4ThreeVector dp = detectorToCT.TransformPoint(G4ThreeVector(0,0,0));

  // Source
  G4ThreeVector s = src->GetAngDist()->GetFocusPointCopy();
  if(src->GetPosDist()->GetPosDisType() == "Point")
    s = src->GetPosDist()->GetCentreCoords(); // point

  m_SourceToCT = sourceToWorld *  m_WorldToCT;
  s = m_SourceToCT.TransformPoint(s);

  // Copy in ITK vectors
  for(int i=0; i<3; i++) {
    detectorRowVector[i] = du[i];
    detectorColVector[i] = dv[i];
    detectorPosition[i] = dp[i];
    primarySourcePosition[i] = s[i];
  }
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
GateHybridForcedDetectionActor::InputImageType::Pointer
GateHybridForcedDetectionActor::ConvertGateImageToITKImage(GateVImageVolume * gateImgVol)
{
  GateImage *gateImg = gateImgVol->GetImage();

  // The direction is not accounted for in Gate.
  InputImageType::SizeType size;
  InputImageType::PointType origin;
  InputImageType::RegionType region;
  InputImageType::SpacingType spacing;
  for(unsigned int i=0; i<3; i++) {
    size[i] = gateImg->GetResolution()[i];
    spacing[i] = gateImg->GetVoxelSize()[i];
    origin[i] = -gateImg->GetHalfSize()[i]+0.5*spacing[i];
  }
  region.SetSize(size);

  itk::ImportImageFilter<InputPixelType, Dimension>::Pointer import;
  import = itk::ImportImageFilter<InputPixelType, Dimension>::New();
  import->SetRegion(region);
  import->SetImportPointer(&*(gateImg->begin()), gateImg->GetNumberOfValues(), false);
  import->SetSpacing(spacing);
  import->SetOrigin(origin);
  TRY_AND_EXIT_ON_ITK_EXCEPTION(import->Update());

  // Get world material
  std::vector<G4Material*> mat;
  gateImgVol->BuildLabelToG4MaterialVector( mat );
  InputPixelType worldMat = mat.size();

  // Pad 1 pixel with world material because interpolation will cut out half a voxel around
  itk::ConstantPadImageFilter<InputImageType, InputImageType>::Pointer pad;
  pad = itk::ConstantPadImageFilter<InputImageType, InputImageType>::New();
  InputImageType::SizeType border;
  border.Fill(1);
  pad->SetPadBound(border);
  pad->SetConstant(worldMat);
  pad->SetInput(import->GetOutput());
  TRY_AND_EXIT_ON_ITK_EXCEPTION(pad->Update());

  InputImageType::Pointer output = pad->GetOutput();
  output->DisconnectPipeline();
  return output;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
GateHybridForcedDetectionActor::InputImageType::Pointer
GateHybridForcedDetectionActor::CreateVoidProjectionImage()
{
  mDetector = GateObjectStore::GetInstance()->FindVolumeCreator(mDetectorName);

  InputImageType::SizeType size;
  size[0] = GetDetectorResolution()[0];
  size[1] = GetDetectorResolution()[1];
  size[2] = 1;

  InputImageType::SpacingType spacing;
  spacing[0] = mDetector->GetHalfDimension(0)*2.0/size[0];
  spacing[1] = mDetector->GetHalfDimension(1)*2.0/size[1];
  spacing[2] = 1.0;

  InputImageType::PointType origin;
  origin[0] = -mDetector->GetHalfDimension(0)+0.5*spacing[0];
  origin[1] = -mDetector->GetHalfDimension(1)+0.5*spacing[1];
  origin[2] = 0.0;

  rtk::ConstantImageSource<InputImageType>::Pointer source;
  source = rtk::ConstantImageSource<InputImageType>::New();
  source->SetSpacing(spacing);
  source->SetOrigin(origin);
  source->SetSize(size);
  TRY_AND_EXIT_ON_ITK_EXCEPTION(source->Update());

  GateHybridForcedDetectionActor::InputImageType::Pointer output;
  output = source->GetOutput();
  output->DisconnectPipeline();

  return output;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void
GateHybridForcedDetectionActor::CreateWaterLUT(const std::vector<double> &energyList,
                                               const std::vector<double> &energyWeightList)
{
  // Get the list of involved processes (Rayleigh, Compton, PhotoElectric)
  G4ParticleDefinition* particle = G4ParticleTable::GetParticleTable()->FindParticle("gamma");
  G4ProcessVector* plist = particle->GetProcessManager()->GetProcessList();
  std::vector<G4String> processNameVector;
  for (G4int j = 0; j < plist->size(); j++) {
    G4ProcessType type = (*plist)[j]->GetProcessType();
    std::string name = (*plist)[j]->GetProcessName();
    if ((type == fElectromagnetic) && (name != "msc")) {
      processNameVector.push_back(name);
    }
  }

  // Create mu data
  std::vector<double> mu(energyList.size(), 0.);
  G4Material * mat = G4NistManager::Instance()->FindOrBuildMaterial(mWaterLUTMaterial);
  double energyWeightDetRespSum = 0.;
  std::vector<double> energyWeightDetResp(energyList.size(), 0.);
  for(unsigned int e=0; e<energyList.size(); e++) {
    for (unsigned int j = 0; j < processNameVector.size(); j++) {
      mu[e] +=
          mEMCalculator->ComputeCrossSectionPerVolume(energyList[e],
                                                      "gamma",
                                                      processNameVector[j],
                                                      mat->GetName());
    }
    energyWeightDetResp[e] = energyWeightList[e] * mEnergyResponseDetector(energyList[e]);
    energyWeightDetRespSum += energyWeightDetResp[e];
  }
  for(unsigned int e=0; e<energyList.size(); e++)
    energyWeightDetResp[e] /= energyWeightDetRespSum;

  const double spacing = 0.1;
  unsigned int n = (unsigned int)floor(1000./spacing);
  typedef itk::Image<double, 1> LUTType;
  LUTType::RegionType region;
  region.SetSize(0, n);
  LUTType::Pointer lengthToAttenuationLUT = LUTType::New();
  lengthToAttenuationLUT->SetRegions(region);
  lengthToAttenuationLUT->Allocate();
  lengthToAttenuationLUT->SetSpacing(&spacing);
  itk::ImageRegionIterator<LUTType> it(lengthToAttenuationLUT, region);
  double prev = itk::NumericTraits<double>::NonpositiveMin();
  double deltaMin = itk::NumericTraits<double>::max();
  for(unsigned int i=0; i<n; i++, ++it) {
    const double length = i*spacing*CLHEP::mm;
    double value = 0.;
    for(unsigned int e=0; e<energyList.size(); e++) {
      value += energyWeightDetResp[e] * exp(-1.*mu[e]*length);
    }
    value = -1. * log(value);
    it.Set(value);
    deltaMin = std::min(deltaMin, value-prev);
    prev = value;
  }
  --it;

  // Take the inverse
  LUTType::Pointer attenuationToLengthLUT = LUTType::New();
  n = (unsigned int)floor(it.Get()/deltaMin);
  region.SetSize(0, n);
  attenuationToLengthLUT->SetRegions(region);
  attenuationToLengthLUT->Allocate();
  attenuationToLengthLUT->SetSpacing(&deltaMin);

  itk::ImageRegionIterator<LUTType> itInv(attenuationToLengthLUT, region);
  itInv.Set(0.);
  ++itInv;
  double currAtt = deltaMin;
  double lengthLeft = 0.;
  double attLeft = 0.;
  it.GoToBegin();
  ++it;
  double attRight = it.Get();
  while(!itInv.IsAtEnd()) {
    while(attRight<currAtt) {
      attLeft = it.Get();
      lengthLeft += spacing;
      ++it;
      attRight = it.Get();
    }
    itInv.Set( ((currAtt-attLeft) * (lengthLeft+spacing) + (attRight-currAtt) * (lengthLeft)) / (attRight-attLeft) );

    // Next
    ++itInv;
    currAtt += deltaMin;
  }

  // Write result
  typedef itk::ImageFileWriter<LUTType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetInput(attenuationToLengthLUT);
  writer->SetFileName(mWaterLUTFilename);
  TRY_AND_EXIT_ON_ITK_EXCEPTION(writer->Update());
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
GateHybridForcedDetectionActor::InputImageType::Pointer
GateHybridForcedDetectionActor::PrimaryFluenceWeighting(const InputImageType::Pointer input)
{
  InputImageType::Pointer output = input;
  if(mSource->GetPosDist()->GetPosDisType() == "Point") {
    GateWarning("Primary fluence is not accounted for with a Point source distribution");
  }
  else if(mSource->GetPosDist()->GetPosDisType() == "Plane") {
    // Check plane source projection probability
    G4ThreeVector sourceCorner1 = mSource->GetPosDist()->GetCentreCoords();
    sourceCorner1[0] -= mSource->GetPosDist()->GetHalfX();
    sourceCorner1[1] -= mSource->GetPosDist()->GetHalfY();
    sourceCorner1 = m_SourceToCT.TransformPoint(sourceCorner1);

    G4ThreeVector sourceCorner2 = mSource->GetPosDist()->GetCentreCoords();
    sourceCorner2[0] += mSource->GetPosDist()->GetHalfX();
    sourceCorner2[1] += mSource->GetPosDist()->GetHalfY();
    sourceCorner2 = m_SourceToCT.TransformPoint(sourceCorner2);

    // Compute source plane corner positions in homogeneous coordinates
    itk::Vector<double, 4> corner1Hom, corner2Hom;
    corner1Hom[0] = sourceCorner1[0];
    corner1Hom[1] = sourceCorner1[1];
    corner1Hom[2] = sourceCorner1[2];
    corner1Hom[3] = 1.;
    corner2Hom[0] = sourceCorner2[0];
    corner2Hom[1] = sourceCorner2[1];
    corner2Hom[2] = sourceCorner2[2];
    corner2Hom[3] = 1.;

    // Project onto detector
    itk::Vector<double, 3> corner1ProjHom, corner2ProjHom;
    corner1ProjHom.SetVnlVector(mGeometry->GetMatrices().back().GetVnlMatrix() * corner1Hom.GetVnlVector());
    corner2ProjHom.SetVnlVector(mGeometry->GetMatrices().back().GetVnlMatrix() * corner2Hom.GetVnlVector());
    corner1ProjHom /= corner1ProjHom[2];
    corner2ProjHom /= corner2ProjHom[2];

    // Convert to non homogeneous coordinates
    InputImageType::PointType corner1Proj, corner2Proj;
    corner1Proj[0] = corner1ProjHom[0];
    corner1Proj[1] = corner1ProjHom[1];
    corner1Proj[2] = 0.;
    corner2Proj[0] = corner2ProjHom[0];
    corner2Proj[1] = corner2ProjHom[1];
    corner2Proj[2] = 0.;

    // Convert to projection indices
    itk::ContinuousIndex<double, 3> corner1Idx, corner2Idx;
    input->TransformPhysicalPointToContinuousIndex<double>(corner1Proj, corner1Idx);
    input->TransformPhysicalPointToContinuousIndex<double>(corner2Proj, corner2Idx);
    if(corner1Idx[0]>corner2Idx[0])
      std::swap(corner1Idx[0], corner2Idx[0]);
    if(corner1Idx[1]>corner2Idx[1])
      std::swap(corner1Idx[1], corner2Idx[1]);

    // Create copy of image normalized by the number of particles and the ratio
    // between source size on the detector and the detector size in pixels
    typedef itk::MultiplyImageFilter<InputImageType, InputImageType> MultiplyType;
    MultiplyType::Pointer mult = MultiplyType::New();
    mult->SetInput(input);
    InputImageType::SizeType size = input->GetLargestPossibleRegion().GetSize();
    mult->SetConstant(mNumberOfEventsInRun /
                      ((corner2Idx[1] - corner1Idx[1]) *
                       (corner2Idx[0] - corner1Idx[0])));
    mult->InPlaceOff();
    TRY_AND_EXIT_ON_ITK_EXCEPTION(mult->Update());

    // Multiply by pixel fraction
    itk::ImageRegionIterator<InputImageType> it(mult->GetOutput(),
                                                mult->GetOutput()->GetLargestPossibleRegion());
    InputImageType::IndexType idx = input->GetLargestPossibleRegion().GetIndex();
    for(unsigned int j=idx[1]; j<size[1]; j++)
      for(unsigned int i=idx[0]; i<size[0]; i++) {
        double maxInfX = std::max<double>(i-0.5, corner1Idx[0]);
        double maxInfY = std::max<double>(j-0.5, corner1Idx[1]);
        double minSupX = std::min<double>(i+0.5, corner2Idx[0]);
        double minSupY = std::min<double>(j+0.5, corner2Idx[1]);
        it.Set(it.Get() *
               std::max<double>(0., minSupX-maxInfX) *
               std::max<double>(0., minSupY-maxInfY));
        ++it;
      }
    output = mult->GetOutput();
  }
  return output;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
G4String
GateHybridForcedDetectionActor::AddPrefix(G4String prefix, G4String filename)
{
  G4String path = itksys::SystemTools::GetFilenamePath(filename);
  G4String name = itksys::SystemTools::GetFilenameName(filename);
  if (path=="")
    path = ".";
  return path+'/'+prefix+name;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
GateHybridForcedDetectionActor::OutputImageType::Pointer
GateHybridForcedDetectionActor::CreateRussianRouletteVoidImage()
{
  InputImageType::SpacingType spacing;
  spacing.Fill(mRussianRouletteSpacing / CLHEP::mm);

  InputImageType::PointType origin;
  InputImageType::SizeType size;
  for(unsigned int i=0; i<3; i++) {
    origin[i] = mGateVolumeImage->GetOrigin()[i] -
                mGateVolumeImage->GetSpacing()[i] * 0.5;
    size[i] = mGateVolumeImage->GetSpacing()[i] *
              mGateVolumeImage->GetLargestPossibleRegion().GetSize()[i] /
              spacing[i];
    size[i] += 1; // One voxel margin to be sure
  }

  rtk::ConstantImageSource<InputImageType>::Pointer source;
  source = rtk::ConstantImageSource<InputImageType>::New();
  source->SetSpacing(spacing);
  source->SetOrigin(origin);
  source->SetSize(size);
  TRY_AND_EXIT_ON_ITK_EXCEPTION(source->Update());

  GateHybridForcedDetectionActor::InputImageType::Pointer output;
  output = source->GetOutput();
  output->DisconnectPipeline();

  return output;
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void
GateHybridForcedDetectionActor::CreatePhaseSpace(const G4String phaseSpaceFilename,
                                                 TFile *&phaseSpaceFile,
                                                 TTree *&phaseSpace)
{
  phaseSpaceFile = NULL;
  if(phaseSpaceFilename != "")
    phaseSpaceFile = new TFile(phaseSpaceFilename,"RECREATE","ROOT file for phase space",9);

  phaseSpace = new TTree("PhaseSpace","Phase space tree of hybrid forced detection actor");
  phaseSpace->Branch("Ekine",  &mInteractionEnergy, "Ekine/D");
  phaseSpace->Branch("Weight", &mInteractionWeight, "Weight/D");
  phaseSpace->Branch("X", &(mInteractionPosition[0]), "X/D");
  phaseSpace->Branch("Y", &(mInteractionPosition[1]), "Y/D");
  phaseSpace->Branch("Z", &(mInteractionPosition[2]), "Z/D");
  phaseSpace->Branch("dX", &(mInteractionDirection[0]), "dX/D");
  phaseSpace->Branch("dY", &(mInteractionDirection[1]), "dY/D");
  phaseSpace->Branch("dZ", &(mInteractionDirection[2]), "dZ/D");
  //  phaseSpace->Branch("ProductionVolume", mInteractionProductionVolume, "ProductionVolume/C");
  //  phaseSpace->Branch("TrackID",&mInteractionTrackId, "TrackID/I");
  phaseSpace->Branch("EventID",&mInteractionEventId, "EventID/I");
  //  phaseSpace->Branch("RunID",&mInteractionRunId, "RunID/I");
  //  phaseSpace->Branch("ProductionProcessTrack", mInteractionProductionProcessTrack, "ProductionProcessTrack/C");
  phaseSpace->Branch("ProductionProcessStep", mInteractionProductionProcessStep, "ProductionProcessStep/C");
  phaseSpace->Branch("TotalContribution", &mInteractionTotalContribution, "TotalContribution/D");
  //  phaseSpace->Branch("InteractionVolume", mInteractionVolume, "Volume/C");
  //  phaseSpace->Branch("Material", mInteractionMaterial, "Material/C");
  phaseSpace->Branch("MaterialZ", &mInteractionZ, "MaterialZ/I");
  phaseSpace->Branch("Order", &mInteractionOrder, "Order/I");
}
//-----------------------------------------------------------------------------
#endif
